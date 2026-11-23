#include "hal/ble_hid.hpp"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#include "esp_hid_common.h"
#include "esp_hidh.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/input.hpp"

static const char* TAG = "ble_hid";

namespace {

constexpr uint16_t APPEARANCE_HID_GENERIC  = 0x03C0;
constexpr uint16_t APPEARANCE_HID_KEYBOARD = 0x03C1;
constexpr uint16_t APPEARANCE_HID_GAMEPAD  = 0x03C4;

struct CandidateDevice {
    bool                valid = false;
    esp_bd_addr_t       bda   = {};
    esp_ble_addr_type_t addr_type = BLE_ADDR_TYPE_PUBLIC;
    int                 score = 0;
    char                name[32] = {};
};

portMUX_TYPE     s_state_mux    = portMUX_INITIALIZER_UNLOCKED;
uint16_t         s_held_buttons = 0;
uint8_t          s_last_keycode = 0;
bool             s_initialized  = false;
bool             s_scanning     = false;
bool             s_connected    = false;
bool             s_connecting   = false;
int8_t           s_battery_pct  = -1;
char             s_dev_name[32] = {};
esp_hidh_dev_t*  s_active_dev   = nullptr;
bool             s_clear_req    = false;

SemaphoreHandle_t s_scan_sem    = nullptr;
CandidateDevice   s_candidate;

bool str_contains_icase(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return false;
    const size_t nlen = strlen(needle);
    for (const char* p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower(static_cast<unsigned char>(p[i])) == tolower(static_cast<unsigned char>(needle[i]))) {
            ++i;
        }
        if (i == nlen) return true;
    }
    return false;
}

bool adv_has_uuid16(const uint8_t* adv_data, uint8_t total_len, esp_ble_adv_data_type ad_type, uint16_t target_uuid) {
    uint8_t len = 0;
    const uint8_t* d = esp_ble_resolve_adv_data_by_type(const_cast<uint8_t*>(adv_data), total_len, ad_type, &len);
    if (!d || len < 2) return false;
    for (uint8_t i = 0; i + 1 < len; i += 2) {
        const uint16_t uuid = static_cast<uint16_t>(d[i]) | (static_cast<uint16_t>(d[i + 1]) << 8);
        if (uuid == target_uuid) return true;
    }
    return false;
}

bool is_bonded_bda(const esp_bd_addr_t bda) {
    int dev_num = esp_ble_get_bond_device_num();
    if (dev_num <= 0) return false;
    auto* list = static_cast<esp_ble_bond_dev_t*>(malloc(sizeof(esp_ble_bond_dev_t) * dev_num));
    if (!list) return false;
    bool found = false;
    if (esp_ble_get_bond_device_list(&dev_num, list) == ESP_OK) {
        for (int i = 0; i < dev_num; ++i) {
            if (memcmp(list[i].bd_addr, bda, sizeof(esp_bd_addr_t)) == 0) {
                found = true;
                break;
            }
        }
    }
    free(list);
    return found;
}

/**
 * @brief Map USB HID Keyboard Usage ID (Page 0x07) to logical ButtonMask.
 *        Supports 8BitDo Micro K-mode factory defaults (arrows + c..p/r/s)
 *        as well as standard keyboard bindings (Enter/Space/Esc/Z/X).
 */
uint16_t map_hid_usage_to_btn(uint8_t usage) {
    switch (usage) {
        // D-Pad: Arrow keys + classic 8BitDo K-mode (c=Up, d=Down, e=Left, f=Right)
        case 0x52: // Arrow Up
        case 0x06: // 'c'
        case 0x1A: // 'w'
            return BTN_UP;

        case 0x51: // Arrow Down
        case 0x07: // 'd'
            return BTN_DOWN;

        case 0x50: // Arrow Left
        case 0x08: // 'e'
            return BTN_LEFT;

        case 0x4F: // Arrow Right
        case 0x09: // 'f'
            return BTN_RIGHT;

        // Face buttons (8BitDo K-mode: g=A, h=X, i=Y, j=B) + standard keys
        case 0x0A: // 'g' (A)
        case 0x28: // Enter
        case 0x2C: // Space
        case 0x1D: // 'z'
            return BTN_A;

        case 0x0D: // 'j' (B)
        case 0x1B: // 'x'
        case 0x2A: // Backspace
            return BTN_B;

        case 0x0B: // 'h' (X)
            return BTN_X;

        case 0x0C: // 'i' (Y)
            return BTN_Y;

        // Shoulder buttons (8BitDo K-mode: k/l/q/r = L1/L2, m/p/s = R1/R2)
        case 0x0E: // 'k' (L1)
        case 0x0F: // 'l' (L2)
        case 0x14: // 'q'
        case 0x15: // 'r'
            return BTN_L;

        case 0x10: // 'm' (R1)
        case 0x13: // 'p' (R2)
        case 0x16: // 's'
            return BTN_R;

        // Start / Select / Escape
        case 0x12: // 'o' (Start)
        case 0x29: // Escape
            return BTN_PAUSE;

        case 0x11: // 'n' (Select)
            return BTN_SELECT;

        default:
            return BTN_NONE;
    }
}

void parse_keyboard_report(const uint8_t* data, uint16_t length) {
    if (!data || length == 0) return;

    uint16_t mask = 0;
    uint8_t  first_key = 0;

    if (length >= 8 && length <= 10) {
        // Standard Boot/Report keyboard format: [modifier, reserved, k0..k5]
        for (uint16_t i = 2; i < length; ++i) {
            const uint8_t k = data[i];
            if (k == 0 || k == 0x01) continue; // 0x01 = rollover error
            if (first_key == 0) first_key = k;
            mask |= map_hid_usage_to_btn(k);
        }
    } else if (length > 10) {
        // NKRO bitmap keyboard report: byte 0 = modifier, bytes 1.. = usage bitmap
        for (uint16_t byte_idx = 1; byte_idx < length; ++byte_idx) {
            const uint8_t b = data[byte_idx];
            if (b == 0) continue;
            for (uint8_t bit = 0; bit < 8; ++bit) {
                if (b & (1U << bit)) {
                    const uint8_t usage = static_cast<uint8_t>((byte_idx - 1) * 8 + bit);
                    if (usage != 0 && first_key == 0) first_key = usage;
                    mask |= map_hid_usage_to_btn(usage);
                }
            }
        }
    } else {
        // Compact report [modifier, k0..]
        for (uint16_t i = 1; i < length; ++i) {
            const uint8_t k = data[i];
            if (k == 0) continue;
            if (first_key == 0) first_key = k;
            mask |= map_hid_usage_to_btn(k);
        }
    }

    portENTER_CRITICAL(&s_state_mux);
    s_held_buttons = mask;
    if (first_key != 0) {
        s_last_keycode = first_key;
    }
    portEXIT_CRITICAL(&s_state_mux);
}

void handle_scan_result(const esp_ble_gap_cb_param_t::ble_scan_result_evt_param& rst) {
    const uint8_t total_len = rst.adv_data_len + rst.scan_rsp_len;

    char name[32] = {};
    uint8_t name_len = 0;
    uint8_t* adv_name = esp_ble_resolve_adv_data_by_type(
        const_cast<uint8_t*>(rst.ble_adv), total_len, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
    if (!adv_name) {
        adv_name = esp_ble_resolve_adv_data_by_type(
            const_cast<uint8_t*>(rst.ble_adv), total_len, ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
    }
    if (adv_name && name_len > 0) {
        const size_t cp = (name_len < sizeof(name) - 1) ? name_len : (sizeof(name) - 1);
        memcpy(name, adv_name, cp);
        name[cp] = '\0';
    }

    uint16_t appearance = 0;
    uint8_t app_len = 0;
    uint8_t* app_d = esp_ble_resolve_adv_data_by_type(
        const_cast<uint8_t*>(rst.ble_adv), total_len, ESP_BLE_AD_TYPE_APPEARANCE, &app_len);
    if (app_d && app_len >= 2) {
        appearance = static_cast<uint16_t>(app_d[0]) | (static_cast<uint16_t>(app_d[1]) << 8);
    }

    const bool has_hid_svc = adv_has_uuid16(rst.ble_adv, total_len, ESP_BLE_AD_TYPE_16SRV_CMPL, ESP_GATT_UUID_HID_SVC) ||
                             adv_has_uuid16(rst.ble_adv, total_len, ESP_BLE_AD_TYPE_16SRV_PART, ESP_GATT_UUID_HID_SVC);
    const bool is_hid_app  = (appearance == APPEARANCE_HID_KEYBOARD) ||
                             (appearance == APPEARANCE_HID_GAMEPAD) ||
                             (appearance == APPEARANCE_HID_GENERIC);
    const bool is_8bitdo   = str_contains_icase(name, "8BitDo");
    const bool is_kbd_name = str_contains_icase(name, "Keyboard");
    const bool bonded      = is_bonded_bda(rst.bda);

    if (!has_hid_svc && !is_hid_app && !is_8bitdo && !is_kbd_name && !bonded) {
        return;
    }

    int score = 10;
    if (has_hid_svc) score += 20;
    if (is_hid_app)  score += 10;
    if (is_8bitdo)   score += 40;
    if (bonded)      score += 50;

    ESP_LOGI(TAG, "HID candidate: name='%s' app=0x%04x hid_svc=%d bonded=%d rssi=%d score=%d",
             name, appearance, has_hid_svc, bonded, rst.rssi, score);

    if (!s_candidate.valid || score > s_candidate.score) {
        s_candidate.valid     = true;
        s_candidate.addr_type = rst.ble_addr_type;
        s_candidate.score     = score;
        memcpy(s_candidate.bda, rst.bda, sizeof(esp_bd_addr_t));
        snprintf(s_candidate.name, sizeof(s_candidate.name), "%s", name[0] ? name : "BLE HID");
    }

    // Fast connect immediately on bonded peer or 8BitDo Micro
    if (bonded || is_8bitdo) {
        esp_ble_gap_stop_scanning();
    }
}

void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    switch (event) {
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            if (s_scan_sem) xSemaphoreGive(s_scan_sem);
            break;

        case ESP_GAP_BLE_SCAN_RESULT_EVT:
            if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
                handle_scan_result(param->scan_rst);
            } else if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
                portENTER_CRITICAL(&s_state_mux);
                s_scanning = false;
                portEXIT_CRITICAL(&s_state_mux);
                if (s_scan_sem) xSemaphoreGive(s_scan_sem);
            }
            break;

        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            portENTER_CRITICAL(&s_state_mux);
            s_scanning = false;
            portEXIT_CRITICAL(&s_state_mux);
            if (s_scan_sem) xSemaphoreGive(s_scan_sem);
            break;

        case ESP_GAP_BLE_SEC_REQ_EVT:
            ESP_LOGI(TAG, "BLE security request accepted");
            esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
            break;

        case ESP_GAP_BLE_NC_REQ_EVT:
            esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
            break;

        case ESP_GAP_BLE_AUTH_CMPL_EVT:
            if (param->ble_security.auth_cmpl.success) {
                ESP_LOGI(TAG, "BLE bonding/auth succeeded");
            } else {
                ESP_LOGW(TAG, "BLE auth failed: 0x%x", param->ble_security.auth_cmpl.fail_reason);
            }
            break;

        default:
            break;
    }
}

void hidh_event_cb(void* /*arg*/, esp_event_base_t /*base*/, int32_t id, void* event_data) {
    const auto event = static_cast<esp_hidh_event_t>(id);
    auto* param      = static_cast<esp_hidh_event_data_t*>(event_data);

    switch (event) {
        case ESP_HIDH_OPEN_EVENT:
            if (param->open.status == ESP_OK && param->open.dev) {
                const char* dname = esp_hidh_dev_name_get(param->open.dev);
                ESP_LOGI(TAG, "HID connected: %s", dname ? dname : "(unknown)");
                portENTER_CRITICAL(&s_state_mux);
                s_active_dev = param->open.dev;
                s_connected  = true;
                s_connecting = false;
                s_scanning   = false;
                snprintf(s_dev_name, sizeof(s_dev_name), "%s", (dname && dname[0]) ? dname : "8BitDo Micro");
                portEXIT_CRITICAL(&s_state_mux);

                const uint8_t* bda = esp_hidh_dev_bda_get(param->open.dev);
                if (bda && !is_bonded_bda(bda)) {
                    esp_ble_set_encryption(const_cast<uint8_t*>(bda), ESP_BLE_SEC_ENCRYPT_NO_MITM);
                }
            } else {
                ESP_LOGW(TAG, "HID open failed (status=%d)", param->open.status);
                portENTER_CRITICAL(&s_state_mux);
                s_active_dev = nullptr;
                s_connected  = false;
                s_connecting = false;
                portEXIT_CRITICAL(&s_state_mux);
            }
            break;

        case ESP_HIDH_BATTERY_EVENT:
            portENTER_CRITICAL(&s_state_mux);
            s_battery_pct = static_cast<int8_t>(param->battery.level);
            portEXIT_CRITICAL(&s_state_mux);
            break;

        case ESP_HIDH_INPUT_EVENT:
            if (param->input.usage == ESP_HID_USAGE_KEYBOARD ||
                param->input.usage == ESP_HID_USAGE_GENERIC) {
                parse_keyboard_report(param->input.data, param->input.length);
            }
            break;

        case ESP_HIDH_CLOSE_EVENT:
            ESP_LOGI(TAG, "HID disconnected (reason=%d)", param->close.reason);
            if (param->close.dev) {
                esp_hidh_dev_free(param->close.dev);
            }
            portENTER_CRITICAL(&s_state_mux);
            s_active_dev   = nullptr;
            s_connected    = false;
            s_connecting   = false;
            s_held_buttons = 0;
            s_battery_pct  = -1;
            s_dev_name[0]  = '\0';
            portEXIT_CRITICAL(&s_state_mux);
            break;

        default:
            break;
    }
}

void perform_clear_bonds() {
    esp_hidh_dev_t* dev = nullptr;
    portENTER_CRITICAL(&s_state_mux);
    dev = s_active_dev;
    portEXIT_CRITICAL(&s_state_mux);

    if (dev) {
        esp_hidh_dev_close(dev);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    int dev_num = esp_ble_get_bond_device_num();
    if (dev_num > 0) {
        auto* list = static_cast<esp_ble_bond_dev_t*>(malloc(sizeof(esp_ble_bond_dev_t) * dev_num));
        if (list && esp_ble_get_bond_device_list(&dev_num, list) == ESP_OK) {
            for (int i = 0; i < dev_num; ++i) {
                esp_ble_remove_bond_device(list[i].bd_addr);
            }
        }
        free(list);
    }
    ESP_LOGI(TAG, "Cleared bonded BLE devices");
}

void ble_hid_task(void* /*arg*/) {
    static esp_ble_scan_params_t scan_params = {
        .scan_type          = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval      = 0x50,
        .scan_window        = 0x30,
        .scan_duplicate     = BLE_SCAN_DUPLICATE_ENABLE,
    };

    while (true) {
        bool do_clear = false;
        bool connected = false;
        portENTER_CRITICAL(&s_state_mux);
        do_clear  = s_clear_req;
        s_clear_req = false;
        connected = s_connected;
        portEXIT_CRITICAL(&s_state_mux);

        if (do_clear) {
            perform_clear_bonds();
            continue;
        }

        if (connected) {
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        // Start a 3-second BLE scan for HID peripherals
        s_candidate = CandidateDevice{};
        xSemaphoreTake(s_scan_sem, 0);

        if (esp_ble_gap_set_scan_params(&scan_params) == ESP_OK) {
            xSemaphoreTake(s_scan_sem, pdMS_TO_TICKS(1000));
        }

        portENTER_CRITICAL(&s_state_mux);
        s_scanning = true;
        portEXIT_CRITICAL(&s_state_mux);

        if (esp_ble_gap_start_scanning(3) == ESP_OK) {
            xSemaphoreTake(s_scan_sem, pdMS_TO_TICKS(3500));
        }

        portENTER_CRITICAL(&s_state_mux);
        s_scanning = false;
        portEXIT_CRITICAL(&s_state_mux);

        if (s_candidate.valid) {
            ESP_LOGI(TAG, "Connecting to '%s'...", s_candidate.name);
            portENTER_CRITICAL(&s_state_mux);
            s_connecting = true;
            snprintf(s_dev_name, sizeof(s_dev_name), "%s", s_candidate.name);
            portEXIT_CRITICAL(&s_state_mux);

            esp_hidh_dev_t* opened = esp_hidh_dev_open(s_candidate.bda, ESP_HID_TRANSPORT_BLE, s_candidate.addr_type);
            if (!opened) {
                ESP_LOGW(TAG, "esp_hidh_dev_open failed");
                portENTER_CRITICAL(&s_state_mux);
                s_connecting = false;
                s_dev_name[0] = '\0';
                portEXIT_CRITICAL(&s_state_mux);
                vTaskDelay(pdMS_TO_TICKS(800));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

} // namespace

bool ble_hid_init() {
    if (s_initialized) return true;

    s_scan_sem = xSemaphoreCreateBinary();
    if (!s_scan_sem) return false;

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_init failed");
        return false;
    }
    if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_enable failed");
        return false;
    }

    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    if (esp_bluedroid_init_with_cfg(&bd_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_bluedroid_init_with_cfg failed");
        return false;
    }
    if (esp_bluedroid_enable() != ESP_OK) {
        ESP_LOGE(TAG, "esp_bluedroid_enable failed");
        return false;
    }

    // Configure BLE JustWorks + Bonding security for 8BitDo Micro K-mode
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t   iocap    = ESP_IO_CAP_NONE;
    uint8_t            init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t            rsp_key  = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t            key_size = 16;

    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(ble_gap_cb));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler));

    esp_hidh_config_t hid_cfg = {
        .callback         = hidh_event_cb,
        .event_stack_size = 4096,
        .callback_arg     = nullptr,
    };
    if (esp_hidh_init(&hid_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidh_init failed");
        return false;
    }

    s_initialized = true;
    xTaskCreatePinnedToCore(ble_hid_task, "ble_hid_task", 4096, nullptr, 3, nullptr, 0);
    ESP_LOGI(TAG, "BLE HID Host initialized (ready for 8BitDo Micro K-mode)");
    return true;
}

uint16_t ble_hid_get_buttons() {
    portENTER_CRITICAL(&s_state_mux);
    const uint16_t mask = s_held_buttons;
    portEXIT_CRITICAL(&s_state_mux);
    return mask;
}

BleHidStatus ble_hid_get_status() {
    BleHidStatus st;
    portENTER_CRITICAL(&s_state_mux);
    st.initialized  = s_initialized;
    st.scanning     = s_scanning;
    st.connected    = s_connected;
    st.battery_pct  = s_battery_pct;
    st.last_keycode = s_last_keycode;
    memcpy(st.device_name, s_dev_name, sizeof(st.device_name));
    portEXIT_CRITICAL(&s_state_mux);
    return st;
}

void ble_hid_clear_bonds() {
    portENTER_CRITICAL(&s_state_mux);
    s_clear_req = true;
    portEXIT_CRITICAL(&s_state_mux);
}
