#include "hal/ble_hid.hpp"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/input.hpp"

static const char* TAG = "ble_hid";

namespace {

constexpr int MAX_SEEN_PER_SCAN = 24;
constexpr int MAX_DISC_SVCS     = 16;
constexpr int MAX_NOTIFY_SUBS   = 16;
constexpr int MAX_REPORT_DESCS  = 8;

struct CandidateDevice {
    bool                valid     = false;
    bool                is_ext    = false;
    esp_bd_addr_t       bda       = {};
    esp_ble_addr_type_t addr_type = BLE_ADDR_TYPE_PUBLIC;
    int                 score     = 0;
    uint16_t            appearance = 0;
    char                name[32]  = {};
};

struct SeenEntry {
    esp_bd_addr_t bda      = {};
    bool          has_name = false;
    bool          has_hid  = false;
};

struct DiscSvc {
    uint16_t      start_handle;
    uint16_t      end_handle;
    esp_bt_uuid_t uuid;
};

struct NotifySub {
    uint16_t char_handle;
    uint16_t cccd_handle;
    uint16_t descr_ref_handle;
    uint16_t uuid16;
    uint16_t svc_uuid16;
    uint8_t  report_id;
    uint8_t  report_type; // 1=Input, 2=Output, 3=Feature
    bool     ref_read;
    bool     is_indicate;
};

enum ReportKind : uint8_t {
    REPORT_KIND_UNKNOWN = 0,
    REPORT_KIND_KEYBOARD,
    REPORT_KIND_GAMEPAD,
    REPORT_KIND_OTHER,
};

struct AxisField {
    bool     valid       = false;
    uint16_t bit_off     = 0;
    uint8_t  bit_size    = 0;
    int32_t  logical_min = 0;
    int32_t  logical_max = 0;
};

struct ReportLayout {
    bool       used            = false;
    uint8_t    report_id       = 0;
    ReportKind kind            = REPORT_KIND_UNKNOWN;
    uint16_t   total_bits      = 0;
    AxisField  hat;
    AxisField  x;
    AxisField  y;
    uint16_t   btn_bit_off[16] = {};
    bool       btn_valid[16]   = {};
    uint8_t    btn_count       = 0;
    uint8_t    max_btn_idx     = 0;
    uint16_t   dpad_bit_off[4] = {}; // Up, Down, Left, Right (0x90..0x93)
    bool       dpad_valid[4]   = {};
};

portMUX_TYPE     s_state_mux         = portMUX_INITIALIZER_UNLOCKED;
uint16_t         s_held_buttons      = 0;
uint16_t         s_held_kbd_buttons  = 0;
uint16_t         s_held_pad_buttons  = 0;
uint8_t          s_last_keycode      = 0;
bool             s_initialized       = false;
bool             s_scanning          = false;
bool             s_connected         = false;
bool             s_connecting        = false;
int8_t           s_battery_pct       = -1;
char             s_dev_name[32]      = {};

esp_gatt_if_t    s_gattc_if          = ESP_GATT_IF_NONE;
uint16_t         s_conn_id           = 0xFFFF;
esp_bd_addr_t    s_remote_bda        = {};
uint16_t         s_peer_appearance   = 0;
bool             s_encrypted         = false;
bool             s_refresh_sent      = false;
bool             s_srvc_searched     = false;
uint32_t         s_connect_start_ms  = 0;

DiscSvc          s_svcs[MAX_DISC_SVCS]         = {};
int              s_svc_count                   = 0;
NotifySub        s_subs[MAX_NOTIFY_SUBS]       = {};
int              s_sub_count                   = 0;
int              s_sub_setup_idx               = 0;
ReportLayout     s_layouts[MAX_REPORT_DESCS]   = {};
int              s_layout_count                = 0;

uint16_t         s_report_map_handle           = 0;
bool             s_report_map_read             = false;
uint16_t         s_battery_handle              = 0;
bool             s_battery_read                = false;
uint16_t         s_ppcp_handle                 = 0;
bool             s_ppcp_read                   = false;
uint16_t         s_dev_name_handle             = 0;
bool             s_dev_name_read               = false;
uint16_t         s_appearance_handle           = 0;
bool             s_appearance_read             = false;

bool             s_clear_req                   = false;
bool             s_scan_params_ready           = false;

SemaphoreHandle_t s_scan_sem                   = nullptr;
CandidateDevice   s_candidate;
SeenEntry         s_seen[MAX_SEEN_PER_SCAN];
int               s_seen_count                 = 0;

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
 *        Supports ZMK/PC gaming keys (Arrows + Z/X/A/S/Q/R/Tab/Esc),
 *        WASD + Enter/Space, and 8BitDo K-mode alphabet keys.
 */
uint16_t map_hid_usage_to_btn(uint8_t usage) {
    switch (usage) {
        // D-Pad: Arrow keys + W + c
        case 0x52: // Arrow Up
        case 0x1A: // 'w'
        case 0x06: // 'c'
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

        // Face buttons: Z/Enter/Space/g=A, X/Backspace/j=B, A/h=X, S/i=Y
        case 0x1D: // 'z'
        case 0x28: // Enter
        case 0x2C: // Space
        case 0x0A: // 'g'
            return BTN_A;

        case 0x1B: // 'x'
        case 0x2A: // Backspace
        case 0x0D: // 'j'
            return BTN_B;

        case 0x04: // 'a'
        case 0x0B: // 'h'
            return BTN_X;

        case 0x16: // 's'
        case 0x0C: // 'i'
            return BTN_Y;

        // Shoulder buttons: Q/[/k/l=L, R/]/m/p=R
        case 0x14: // 'q'
        case 0x2F: // '['
        case 0x0E: // 'k'
        case 0x0F: // 'l'
            return BTN_L;

        case 0x15: // 'r'
        case 0x30: // ']'
        case 0x10: // 'm'
        case 0x13: // 'p'
            return BTN_R;

        // Start / Select
        case 0x29: // Escape
        case 0x12: // 'o'
            return BTN_PAUSE;

        case 0x2B: // Tab
        case 0x11: // 'n'
            return BTN_SELECT;

        default:
            return BTN_NONE;
    }
}

ReportLayout* get_or_add_layout(uint8_t report_id) {
    for (int i = 0; i < s_layout_count; ++i) {
        if (s_layouts[i].report_id == report_id) {
            return &s_layouts[i];
        }
    }
    if (s_layout_count < MAX_REPORT_DESCS) {
        ReportLayout* l = &s_layouts[s_layout_count++];
        *l              = ReportLayout{};
        l->used         = true;
        l->report_id    = report_id;
        return l;
    }
    return nullptr;
}

const ReportLayout* find_layout_by_id(uint8_t report_id) {
    for (int i = 0; i < s_layout_count; ++i) {
        if (s_layouts[i].used && s_layouts[i].report_id == report_id) {
            return &s_layouts[i];
        }
    }
    if (s_layout_count == 1 && s_layouts[0].used) {
        return &s_layouts[0];
    }
    return nullptr;
}

int32_t read_hid_signed(const uint8_t* p, uint8_t size) {
    if (size == 1) return static_cast<int8_t>(p[0]);
    if (size == 2) return static_cast<int16_t>(p[0] | (p[1] << 8));
    if (size == 4) {
        return static_cast<int32_t>(
            static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
            (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24));
    }
    return 0;
}

uint32_t read_hid_unsigned(const uint8_t* p, uint8_t size) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < size && i < 4; ++i) {
        v |= static_cast<uint32_t>(p[i]) << (i * 8);
    }
    return v;
}

/**
 * @brief Parse BLE HID Report Map (0x2a4b) so both Keyboard and Gamepad
 *        reports (even in composite descriptors) are classified and decoded.
 */
void parse_hid_report_map(const uint8_t* map, uint16_t len) {
    s_layout_count = 0;
    if (!map || len == 0) return;

    struct GlobalState {
        uint16_t usage_page  = 0;
        int32_t  logical_min = 0;
        int32_t  logical_max = 0;
        uint32_t report_size = 0;
        uint32_t report_cnt  = 0;
        uint8_t  report_id   = 0;
    };

    GlobalState g{};
    GlobalState stack[4];
    int sp = 0;

    uint32_t local_usages[32] = {};
    int      local_count      = 0;
    uint32_t usage_min        = 0;
    uint32_t usage_max        = 0;
    bool     has_usage_range  = false;

    int        coll_depth = 0;
    ReportKind app_kind   = REPORT_KIND_UNKNOWN;

    uint16_t idx = 0;
    while (idx < len) {
        const uint8_t hdr = map[idx++];
        if (hdr == 0xFE) { // Long item
            if (idx + 1 >= len) break;
            const uint8_t data_len = map[idx];
            idx += 2 + data_len;
            continue;
        }

        const uint8_t bSize = (hdr & 0x03) == 3 ? 4 : (hdr & 0x03);
        const uint8_t bType = (hdr >> 2) & 0x03;
        const uint8_t bTag  = (hdr >> 4) & 0x0F;
        if (idx + bSize > len) break;

        const uint8_t* d    = &map[idx];
        const uint32_t uval = read_hid_unsigned(d, bSize);
        const int32_t  sval = read_hid_signed(d, bSize);
        idx += bSize;

        if (bType == 0) { // Main
            if (bTag == 0x0A) { // Collection
                if (coll_depth == 0 && uval == 1) { // Application collection
                    const uint32_t app_usage = (local_count > 0) ? local_usages[0] : 0;
                    if (g.usage_page == 0x01 && app_usage == 0x06) {
                        app_kind = REPORT_KIND_KEYBOARD;
                    } else if (g.usage_page == 0x01 && (app_usage == 0x04 || app_usage == 0x05)) {
                        app_kind = REPORT_KIND_GAMEPAD;
                    } else {
                        app_kind = REPORT_KIND_OTHER;
                    }
                }
                ++coll_depth;
            } else if (bTag == 0x0C) { // End Collection
                if (coll_depth > 0) --coll_depth;
                if (coll_depth == 0) app_kind = REPORT_KIND_UNKNOWN;
            } else if (bTag == 0x08) { // Input
                ReportLayout* l = get_or_add_layout(g.report_id);
                if (l) {
                    if (l->kind == REPORT_KIND_UNKNOWN) {
                        l->kind = app_kind;
                    }
                    const bool is_const = (uval & 0x01) != 0;
                    if (!is_const && app_kind == REPORT_KIND_GAMEPAD) {
                        for (uint32_t c = 0; c < g.report_cnt; ++c) {
                            uint32_t u = 0;
                            if (has_usage_range) {
                                u = usage_min + c;
                                if (u > usage_max) u = usage_max;
                            } else if (local_count > 0) {
                                const int ui = (static_cast<int>(c) < local_count) ? static_cast<int>(c) : (local_count - 1);
                                u = local_usages[ui];
                            }
                            const uint16_t bit_off = l->total_bits + static_cast<uint16_t>(c * g.report_size);
                            if (g.usage_page == 0x01) {
                                if (u == 0x39 && !l->hat.valid) {
                                    l->hat = AxisField{true, bit_off, static_cast<uint8_t>(g.report_size), g.logical_min, g.logical_max};
                                } else if (u == 0x30 && !l->x.valid) {
                                    l->x = AxisField{true, bit_off, static_cast<uint8_t>(g.report_size), g.logical_min, g.logical_max};
                                } else if (u == 0x31 && !l->y.valid) {
                                    l->y = AxisField{true, bit_off, static_cast<uint8_t>(g.report_size), g.logical_min, g.logical_max};
                                } else if (u >= 0x90 && u <= 0x93) {
                                    const uint8_t di = static_cast<uint8_t>(u - 0x90);
                                    l->dpad_bit_off[di] = bit_off;
                                    l->dpad_valid[di]   = true;
                                }
                            } else if (g.usage_page == 0x09 && u >= 1 && u <= 16) {
                                const uint8_t bi = static_cast<uint8_t>(u - 1);
                                l->btn_bit_off[bi] = bit_off;
                                l->btn_valid[bi]   = true;
                                ++l->btn_count;
                                if (bi > l->max_btn_idx) l->max_btn_idx = bi;
                            }
                        }
                    }
                    l->total_bits += static_cast<uint16_t>(g.report_size * g.report_cnt);
                }
            }
            local_count     = 0;
            has_usage_range = false;
        } else if (bType == 1) { // Global
            switch (bTag) {
                case 0x00: g.usage_page  = static_cast<uint16_t>(uval); break;
                case 0x01: g.logical_min = sval; break;
                case 0x02: g.logical_max = sval; break;
                case 0x07: g.report_size = uval; break;
                case 0x08: g.report_id   = static_cast<uint8_t>(uval); break;
                case 0x09: g.report_cnt  = uval; break;
                case 0x0A: if (sp < 4) stack[sp++] = g; break;
                case 0x0B: if (sp > 0) g = stack[--sp]; break;
                default: break;
            }
        } else if (bType == 2) { // Local
            if (bTag == 0x00) { // Usage
                if (local_count < 32) local_usages[local_count++] = uval;
            } else if (bTag == 0x01) { // Usage Min
                usage_min       = uval;
                has_usage_range = true;
            } else if (bTag == 0x02) { // Usage Max
                usage_max       = uval;
                has_usage_range = true;
            }
        }
    }

    for (int i = 0; i < s_layout_count; ++i) {
        const ReportLayout& l = s_layouts[i];
        ESP_LOGI(TAG, "  ReportMap[id=%u]: kind=%u bits=%u hat=%d x=%d y=%d btns=%u",
                 l.report_id, l.kind, l.total_bits, l.hat.valid, l.x.valid, l.y.valid, l.btn_count);
    }
}

uint32_t extract_bits_u(const uint8_t* data, uint16_t len, uint16_t bit_off, uint8_t bit_size) {
    if (bit_size == 0 || bit_size > 32) return 0;
    uint32_t val = 0;
    for (uint8_t b = 0; b < bit_size; ++b) {
        const uint16_t bi  = static_cast<uint16_t>((bit_off + b) / 8);
        const uint8_t  bit = static_cast<uint8_t>((bit_off + b) % 8);
        if (bi >= len) break;
        if (data[bi] & (1U << bit)) {
            val |= (1UL << b);
        }
    }
    return val;
}

int32_t extract_bits_s(const uint8_t* data, uint16_t len, const AxisField& f) {
    const uint32_t raw = extract_bits_u(data, len, f.bit_off, f.bit_size);
    if (f.logical_min < 0 && f.bit_size > 0 && f.bit_size < 32) {
        const uint32_t sign_bit = 1UL << (f.bit_size - 1);
        if (raw & sign_bit) {
            return static_cast<int32_t>(raw | (~((1UL << f.bit_size) - 1UL)));
        }
    }
    return static_cast<int32_t>(raw);
}

void parse_keyboard_report(const uint8_t* data, uint16_t length) {
    if (!data || length == 0) return;

    uint16_t mask      = 0;
    uint8_t  first_key = 0;

    if (length >= 8 && length <= 10) {
        // Standard 6KRO BLE GATT keyboard report: [mod, reserved, k0..k5]
        for (uint16_t i = 2; i < length; ++i) {
            const uint8_t k = data[i];
            if (k == 0 || k == 0x01) continue;
            if (first_key == 0) first_key = k;
            mask |= map_hid_usage_to_btn(k);
        }
    } else if (length > 10) {
        // NKRO bitmap keyboard report: [mod, bitmap bytes 0..N]
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
        for (uint16_t i = 1; i < length; ++i) {
            const uint8_t k = data[i];
            if (k == 0) continue;
            if (first_key == 0) first_key = k;
            mask |= map_hid_usage_to_btn(k);
        }
    }

    portENTER_CRITICAL(&s_state_mux);
    s_held_kbd_buttons = mask;
    s_held_buttons     = s_held_kbd_buttons | s_held_pad_buttons;
    if (first_key != 0) {
        s_last_keycode = first_key;
    }
    portEXIT_CRITICAL(&s_state_mux);
}

void parse_gamepad_report(const ReportLayout* layout, const uint8_t* data, uint16_t length) {
    if (!data || length == 0) return;

    uint16_t mask      = 0;
    uint8_t  first_btn = 0;

    if (layout && layout->kind == REPORT_KIND_GAMEPAD) {
        if (layout->hat.valid) {
            const int32_t hat = static_cast<int32_t>(
                extract_bits_u(data, length, layout->hat.bit_off, layout->hat.bit_size)) - layout->hat.logical_min;
            if (hat >= 0 && hat <= 7) {
                if (hat == 7 || hat == 0 || hat == 1) mask |= BTN_UP;
                if (hat == 1 || hat == 2 || hat == 3) mask |= BTN_RIGHT;
                if (hat == 3 || hat == 4 || hat == 5) mask |= BTN_DOWN;
                if (hat == 5 || hat == 6 || hat == 7) mask |= BTN_LEFT;
            }
        }
        if (layout->x.valid && layout->x.logical_max > layout->x.logical_min) {
            const int32_t xv   = extract_bits_s(data, length, layout->x);
            const int32_t mid  = (layout->x.logical_min + layout->x.logical_max) / 2;
            const int32_t dead = (layout->x.logical_max - layout->x.logical_min) / 4;
            if (xv < mid - dead) mask |= BTN_LEFT;
            if (xv > mid + dead) mask |= BTN_RIGHT;
        }
        if (layout->y.valid && layout->y.logical_max > layout->y.logical_min) {
            const int32_t yv   = extract_bits_s(data, length, layout->y);
            const int32_t mid  = (layout->y.logical_min + layout->y.logical_max) / 2;
            const int32_t dead = (layout->y.logical_max - layout->y.logical_min) / 4;
            if (yv < mid - dead) mask |= BTN_UP;
            if (yv > mid + dead) mask |= BTN_DOWN;
        }
        static constexpr uint16_t kDpadMap[4] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT};
        for (int i = 0; i < 4; ++i) {
            if (layout->dpad_valid[i] && extract_bits_u(data, length, layout->dpad_bit_off[i], 1)) {
                mask |= kDpadMap[i];
            }
        }

        // Support both sparse Android/Xbox HID (gaps at Btn3/Btn6) and contiguous 1..12 button layouts
        const bool sparse_xbox = (layout->max_btn_idx >= 10 && (!layout->btn_valid[2] || !layout->btn_valid[5]));
        static constexpr uint16_t kXboxBtnMap[16] = {
            BTN_A, BTN_B, BTN_NONE, BTN_X, BTN_Y, BTN_NONE, BTN_L, BTN_R,
            BTN_L, BTN_R, BTN_SELECT, BTN_PAUSE, BTN_PAUSE, BTN_NONE, BTN_NONE, BTN_NONE,
        };
        static constexpr uint16_t kCompactBtnMap[16] = {
            BTN_A, BTN_B, BTN_X, BTN_Y, BTN_L, BTN_R, BTN_L, BTN_R,
            BTN_SELECT, BTN_PAUSE, BTN_SELECT, BTN_PAUSE, BTN_NONE, BTN_NONE, BTN_NONE, BTN_NONE,
        };
        const uint16_t* btn_map = sparse_xbox ? kXboxBtnMap : kCompactBtnMap;
        for (int i = 0; i < 16; ++i) {
            if (layout->btn_valid[i] && extract_bits_u(data, length, layout->btn_bit_off[i], 1)) {
                if (first_btn == 0) first_btn = static_cast<uint8_t>(0xA0 + i + 1);
                mask |= btn_map[i];
            }
        }
    } else if (length >= 3) {
        // Fallback generic gamepad: [buttons_lo, buttons_hi, hat/dpad]
        const uint16_t btns = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
        if (btns & (1U << 0)) mask |= BTN_A;
        if (btns & (1U << 1)) mask |= BTN_B;
        if (btns & (1U << 2)) mask |= BTN_X;
        if (btns & (1U << 3)) mask |= BTN_Y;
        if (btns & (1U << 4)) mask |= BTN_L;
        if (btns & (1U << 5)) mask |= BTN_R;
        if (btns & (1U << 6)) mask |= BTN_SELECT;
        if (btns & (1U << 7)) mask |= BTN_PAUSE;
        if (btns && first_btn == 0) first_btn = 0xA0;
    }

    portENTER_CRITICAL(&s_state_mux);
    s_held_pad_buttons = mask;
    s_held_buttons     = s_held_kbd_buttons | s_held_pad_buttons;
    if (first_btn != 0) {
        s_last_keycode = first_btn;
    }
    portEXIT_CRITICAL(&s_state_mux);
}

void handle_ext_adv_report(const esp_ble_gap_ext_adv_report_t& p) {
    const uint8_t* adv_data = p.adv_data;
    const uint8_t  total    = p.adv_data_len;
    const bool     is_ext   = !(p.event_type & ESP_BLE_GAP_SET_EXT_ADV_PROP_LEGACY);

    char name[32] = {};
    uint8_t nlen = 0;
    const uint8_t* nptr = esp_ble_resolve_adv_data_by_type(
        const_cast<uint8_t*>(adv_data), total, ESP_BLE_AD_TYPE_NAME_CMPL, &nlen);
    if (!nptr || nlen == 0) {
        nptr = esp_ble_resolve_adv_data_by_type(
            const_cast<uint8_t*>(adv_data), total, ESP_BLE_AD_TYPE_NAME_SHORT, &nlen);
    }
    if (nptr && nlen > 0) {
        const size_t cp = (nlen < sizeof(name) - 1) ? nlen : (sizeof(name) - 1);
        memcpy(name, nptr, cp);
        name[cp] = '\0';
    }

    uint16_t appearance = 0;
    uint8_t alen = 0;
    const uint8_t* aptr = esp_ble_resolve_adv_data_by_type(
        const_cast<uint8_t*>(adv_data), total, ESP_BLE_AD_TYPE_APPEARANCE, &alen);
    if (aptr && alen >= 2) {
        appearance = static_cast<uint16_t>(aptr[0] | (aptr[1] << 8));
    }

    const bool has_hid =
        adv_has_uuid16(adv_data, total, ESP_BLE_AD_TYPE_16SRV_CMPL, ESP_GATT_UUID_HID_SVC) ||
        adv_has_uuid16(adv_data, total, ESP_BLE_AD_TYPE_16SRV_PART, ESP_GATT_UUID_HID_SVC);
    const bool is_hid_app = (appearance >= 0x03C0 && appearance <= 0x03CF);
    const bool is_pad_name =
        str_contains_icase(name, "GConsole") ||
        str_contains_icase(name, "Pad12") ||
        str_contains_icase(name, "Gamepad") ||
        str_contains_icase(name, "Controller") ||
        str_contains_icase(name, "Keyboard");
    const bool bonded = is_bonded_bda(p.addr);

    SeenEntry* seen = nullptr;
    for (int i = 0; i < s_seen_count; ++i) {
        if (memcmp(s_seen[i].bda, p.addr, sizeof(esp_bd_addr_t)) == 0) {
            seen = &s_seen[i];
            break;
        }
    }
    const bool is_new_info = (seen == nullptr) ||
                             (!seen->has_name && name[0] != '\0') ||
                             (!seen->has_hid && (has_hid || is_hid_app));
    if (seen == nullptr && s_seen_count < MAX_SEEN_PER_SCAN) {
        seen = &s_seen[s_seen_count++];
        memcpy(seen->bda, p.addr, sizeof(esp_bd_addr_t));
    }
    if (seen) {
        if (name[0] != '\0') seen->has_name = true;
        if (has_hid || is_hid_app) seen->has_hid = true;
    }
    if (is_new_info && (has_hid || is_hid_app || is_pad_name || bonded || name[0] != '\0')) {
        ESP_LOGI(TAG, "BLE seen %02x:%02x:%02x:%02x:%02x:%02x type=%u ext=%d rssi=%d app=0x%04x hid=%d bonded=%d name='%s'",
                 p.addr[0], p.addr[1], p.addr[2], p.addr[3], p.addr[4], p.addr[5],
                 p.addr_type, is_ext, p.rssi, appearance, has_hid, bonded, name);
    }

    // Explicitly ignore 8BitDo's non-HID Ultimate Software channel ('80EL')
    if (str_contains_icase(name, "80EL") && !has_hid) {
        return;
    }

    if (!has_hid && !is_hid_app && !is_pad_name && !bonded) {
        return;
    }

    int score = 10;
    if (has_hid)    score += 35;
    if (is_hid_app) score += 20;
    if (is_pad_name) score += 25;
    if (bonded)     score += 40;
    if (str_contains_icase(name, "GConsole") || str_contains_icase(name, "Pad12")) score += 30;

    if (!s_candidate.valid || score > s_candidate.score) {
        s_candidate.valid      = true;
        s_candidate.is_ext     = is_ext;
        memcpy(s_candidate.bda, p.addr, 6);
        s_candidate.addr_type  = static_cast<esp_ble_addr_type_t>(p.addr_type);
        s_candidate.score      = score;
        if (appearance != 0) s_candidate.appearance = appearance;
        snprintf(s_candidate.name, sizeof(s_candidate.name), "%s", name[0] ? name : "BLE HID");
    }

    if (s_candidate.valid && (bonded || (has_hid && is_hid_app))) {
        xSemaphoreGive(s_scan_sem);
    }
}

void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    switch (event) {
        case ESP_GAP_BLE_SET_EXT_SCAN_PARAMS_COMPLETE_EVT:
            ESP_LOGI(TAG, "BLE ext scan params set (status=%d)", param->set_ext_scan_params.status);
            s_scan_params_ready = (param->set_ext_scan_params.status == ESP_BT_STATUS_SUCCESS);
            if (s_scan_sem) xSemaphoreGive(s_scan_sem);
            break;

        case ESP_GAP_BLE_EXT_ADV_REPORT_EVT:
            handle_ext_adv_report(param->ext_adv_report.params);
            break;

        case ESP_GAP_BLE_EXT_SCAN_STOP_COMPLETE_EVT:
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
                ESP_LOGI(TAG, "BLE bonding/auth succeeded (mode=%d)",
                         param->ble_security.auth_cmpl.auth_mode);
                bool need_refresh = false;
                portENTER_CRITICAL(&s_state_mux);
                s_encrypted = true;
                if (s_conn_id != 0xFFFF && !s_refresh_sent) {
                    s_refresh_sent = true;
                    need_refresh = true;
                }
                portEXIT_CRITICAL(&s_state_mux);
                if (need_refresh) {
                    esp_ble_gattc_send_mtu_req(s_gattc_if, s_conn_id);
                    const esp_err_t rerr = esp_ble_gattc_cache_refresh(s_remote_bda);
                    if (rerr != ESP_OK && s_gattc_if != ESP_GATT_IF_NONE) {
                        s_srvc_searched = true;
                        esp_ble_gattc_search_service(s_gattc_if, s_conn_id, nullptr);
                    }
                }
            } else {
                ESP_LOGW(TAG, "BLE auth failed: 0x%x", param->ble_security.auth_cmpl.fail_reason);
                if (s_gattc_if != ESP_GATT_IF_NONE && s_conn_id != 0xFFFF) {
                    esp_ble_gattc_close(s_gattc_if, s_conn_id);
                }
            }
            break;

        default:
            break;
    }
}

void format_uuid(const esp_bt_uuid_t& u, char* buf, size_t len) {
    if (u.len == ESP_UUID_LEN_16) {
        snprintf(buf, len, "0x%04x", u.uuid.uuid16);
    } else if (u.len == ESP_UUID_LEN_32) {
        snprintf(buf, len, "0x%08lx", (unsigned long)u.uuid.uuid32);
    } else {
        const uint8_t* p = u.uuid.uuid128;
        snprintf(buf, len, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 p[15], p[14], p[13], p[12], p[11], p[10], p[9], p[8],
                 p[7], p[6], p[5], p[4], p[3], p[2], p[1], p[0]);
    }
}

void advance_gattc_setup(esp_gatt_if_t gattc_if, uint16_t conn_id) {
    for (int i = 0; i < s_sub_count; ++i) {
        if (s_subs[i].descr_ref_handle != 0 && !s_subs[i].ref_read) {
            s_subs[i].ref_read = true;
            if (esp_ble_gattc_read_char_descr(gattc_if, conn_id, s_subs[i].descr_ref_handle,
                                              ESP_GATT_AUTH_REQ_NONE) == ESP_OK) {
                return;
            }
        }
    }
    if (s_dev_name_handle != 0 && !s_dev_name_read) {
        s_dev_name_read = true;
        if (esp_ble_gattc_read_char(gattc_if, conn_id, s_dev_name_handle,
                                    ESP_GATT_AUTH_REQ_NONE) == ESP_OK) {
            return;
        }
    }
    if (s_appearance_handle != 0 && !s_appearance_read) {
        s_appearance_read = true;
        if (esp_ble_gattc_read_char(gattc_if, conn_id, s_appearance_handle,
                                    ESP_GATT_AUTH_REQ_NONE) == ESP_OK) {
            return;
        }
    }
    if (s_ppcp_handle != 0 && !s_ppcp_read) {
        s_ppcp_read = true;
        if (esp_ble_gattc_read_char(gattc_if, conn_id, s_ppcp_handle,
                                    ESP_GATT_AUTH_REQ_NONE) == ESP_OK) {
            return;
        }
    }
    if (s_report_map_handle != 0 && !s_report_map_read) {
        s_report_map_read = true;
        if (esp_ble_gattc_read_char(gattc_if, conn_id, s_report_map_handle,
                                    ESP_GATT_AUTH_REQ_NONE) == ESP_OK) {
            return;
        }
    }
    if (s_battery_handle != 0 && !s_battery_read) {
        s_battery_read = true;
        if (esp_ble_gattc_read_char(gattc_if, conn_id, s_battery_handle,
                                    ESP_GATT_AUTH_REQ_NONE) == ESP_OK) {
            return;
        }
    }
    while (s_sub_setup_idx < s_sub_count) {
        const int idx = s_sub_setup_idx++;
        if (s_subs[idx].cccd_handle != 0) {
            uint8_t cccd_val[2] = {
                static_cast<uint8_t>(s_subs[idx].is_indicate ? 0x02 : 0x01),
                0x00
            };
            if (esp_ble_gattc_write_char_descr(gattc_if, conn_id, s_subs[idx].cccd_handle,
                                               sizeof(cccd_val), cccd_val,
                                               ESP_GATT_WRITE_TYPE_RSP,
                                               ESP_GATT_AUTH_REQ_NONE) == ESP_OK) {
                return;
            }
        }
    }

    int hid_input_subs = 0;
    for (int i = 0; i < s_sub_count; ++i) {
        if (s_subs[i].svc_uuid16 == ESP_GATT_UUID_HID_SVC &&
            (s_subs[i].report_type == 1 || s_subs[i].report_type == 0)) {
            ++hid_input_subs;
        }
    }

    if (hid_input_subs > 0) {
        ESP_LOGI(TAG, "BLE HID ready! Subscribed to %d HID input report handle(s)", hid_input_subs);
        portENTER_CRITICAL(&s_state_mux);
        s_connected  = true;
        s_connecting = false;
        s_scanning   = false;
        portEXIT_CRITICAL(&s_state_mux);
    } else {
        ESP_LOGW(TAG, "HID Service (0x1812) input reports not found; closing");
        esp_ble_gattc_close(gattc_if, conn_id);
    }
}

void ble_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t* param) {
    switch (event) {
        case ESP_GATTC_REG_EVT:
            if (param->reg.status == ESP_GATT_OK) {
                s_gattc_if = gattc_if;
                ESP_LOGI(TAG, "GATTC registered if=%u", gattc_if);
            } else {
                ESP_LOGE(TAG, "GATTC reg failed status=%d", param->reg.status);
            }
            break;

        case ESP_GATTC_OPEN_EVT:
            if (param->open.status == ESP_GATT_OK) {
                ESP_LOGI(TAG, "GATTC OPEN ok conn_id=%u -> encrypting", param->open.conn_id);
                portENTER_CRITICAL(&s_state_mux);
                s_conn_id           = param->open.conn_id;
                memcpy(s_remote_bda, param->open.remote_bda, 6);
                s_encrypted         = false;
                s_refresh_sent      = false;
                s_srvc_searched     = false;
                s_svc_count         = 0;
                s_sub_count         = 0;
                s_sub_setup_idx     = 0;
                s_layout_count      = 0;
                s_report_map_handle = 0;
                s_report_map_read   = false;
                s_battery_handle    = 0;
                s_battery_read      = false;
                s_ppcp_handle       = 0;
                s_ppcp_read         = false;
                s_dev_name_handle   = 0;
                s_dev_name_read     = false;
                s_appearance_handle = 0;
                s_appearance_read   = false;
                portEXIT_CRITICAL(&s_state_mux);
                esp_ble_set_encryption(param->open.remote_bda, ESP_BLE_SEC_ENCRYPT_NO_MITM);
            } else {
                ESP_LOGW(TAG, "GATTC OPEN failed status=0x%x", param->open.status);
                portENTER_CRITICAL(&s_state_mux);
                s_conn_id     = 0xFFFF;
                s_connected   = false;
                s_connecting  = false;
                s_dev_name[0] = '\0';
                portEXIT_CRITICAL(&s_state_mux);
            }
            break;

        case ESP_GATTC_CFG_MTU_EVT:
            ESP_LOGI(TAG, "GATTC MTU=%u status=0x%x", param->cfg_mtu.mtu, param->cfg_mtu.status);
            break;

        case ESP_GATTC_DIS_SRVC_CMPL_EVT:
            if (s_encrypted && !s_srvc_searched && param->dis_srvc_cmpl.status == ESP_GATT_OK) {
                s_srvc_searched = true;
                s_svc_count     = 0;
                s_sub_count     = 0;
                s_sub_setup_idx = 0;
                esp_ble_gattc_search_service(gattc_if, param->dis_srvc_cmpl.conn_id, nullptr);
            }
            break;

        case ESP_GATTC_SRVC_CHG_EVT:
            ESP_LOGI(TAG, "GATTC SRVC_CHG received");
            if (s_encrypted && s_conn_id != 0xFFFF) {
                s_srvc_searched = false;
                esp_ble_gattc_cache_refresh(s_remote_bda);
            }
            break;

        case ESP_GATTC_SEARCH_RES_EVT:
            if (s_svc_count < MAX_DISC_SVCS) {
                s_svcs[s_svc_count++] = DiscSvc{
                    param->search_res.start_handle,
                    param->search_res.end_handle,
                    param->search_res.srvc_id.uuid,
                };
            }
            break;

        case ESP_GATTC_SEARCH_CMPL_EVT: {
            ESP_LOGI(TAG, "GATTC SEARCH_CMPL status=%d services=%d",
                     param->search_cmpl.status, s_svc_count);
            for (int s = 0; s < s_svc_count; ++s) {
                const DiscSvc& svc     = s_svcs[s];
                const uint16_t svc_u16 = (svc.uuid.len == ESP_UUID_LEN_16) ? svc.uuid.uuid.uuid16 : 0;
                char sbuf[40];
                format_uuid(svc.uuid, sbuf, sizeof(sbuf));
                ESP_LOGI(TAG, "  SVC %s [0x%04x..0x%04x]", sbuf, svc.start_handle, svc.end_handle);

                esp_gattc_char_elem_t chars[24];
                uint16_t char_cnt = 24;
                if (esp_ble_gattc_get_all_char(gattc_if, param->search_cmpl.conn_id,
                                               svc.start_handle, svc.end_handle,
                                               chars, &char_cnt, 0) != ESP_GATT_OK) {
                    continue;
                }
                for (uint16_t c = 0; c < char_cnt; ++c) {
                    const uint16_t cu16 = (chars[c].uuid.len == ESP_UUID_LEN_16) ? chars[c].uuid.uuid.uuid16 : 0;

                    uint16_t cccd_handle = 0;
                    uint16_t ref_handle  = 0;
                    esp_gattc_descr_elem_t descrs[8];
                    uint16_t dcnt = 8;
                    if (esp_ble_gattc_get_all_descr(gattc_if, param->search_cmpl.conn_id,
                                                    chars[c].char_handle, descrs, &dcnt, 0) == ESP_GATT_OK) {
                        for (uint16_t d = 0; d < dcnt; ++d) {
                            if (descrs[d].uuid.len == ESP_UUID_LEN_16) {
                                if (descrs[d].uuid.uuid.uuid16 == ESP_GATT_UUID_CHAR_CLIENT_CONFIG) {
                                    cccd_handle = descrs[d].handle;
                                } else if (descrs[d].uuid.uuid.uuid16 == ESP_GATT_UUID_RPT_REF_DESCR) {
                                    ref_handle = descrs[d].handle;
                                }
                            }
                        }
                    }

                    if (svc_u16 == ESP_GATT_UUID_HID_SVC && cu16 == ESP_GATT_UUID_HID_REPORT_MAP &&
                        (chars[c].properties & ESP_GATT_CHAR_PROP_BIT_READ)) {
                        s_report_map_handle = chars[c].char_handle;
                    }
                    if (svc_u16 == 0x1800 && cu16 == 0x2a00 &&
                        (chars[c].properties & ESP_GATT_CHAR_PROP_BIT_READ)) {
                        s_dev_name_handle = chars[c].char_handle;
                    }
                    if (svc_u16 == 0x1800 && cu16 == 0x2a01 &&
                        (chars[c].properties & ESP_GATT_CHAR_PROP_BIT_READ)) {
                        s_appearance_handle = chars[c].char_handle;
                    }
                    if (svc_u16 == 0x1800 && cu16 == 0x2a04 &&
                        (chars[c].properties & ESP_GATT_CHAR_PROP_BIT_READ)) {
                        s_ppcp_handle = chars[c].char_handle;
                    }
                    if (svc_u16 == ESP_GATT_UUID_BATTERY_SERVICE_SVC && cu16 == ESP_GATT_UUID_BATTERY_LEVEL &&
                        (chars[c].properties & ESP_GATT_CHAR_PROP_BIT_READ)) {
                        s_battery_handle = chars[c].char_handle;
                    }

                    const bool is_srv_chg = (svc_u16 == 0x1801 && cu16 == 0x2a05 &&
                                             (chars[c].properties & ESP_GATT_CHAR_PROP_BIT_INDICATE));
                    const bool is_hid_ntf = (svc_u16 == ESP_GATT_UUID_HID_SVC &&
                                             (cu16 == ESP_GATT_UUID_HID_REPORT ||
                                              cu16 == ESP_GATT_UUID_HID_BT_KB_INPUT) &&
                                             (chars[c].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY));
                    if ((is_srv_chg || is_hid_ntf) && s_sub_count < MAX_NOTIFY_SUBS) {
                        s_subs[s_sub_count++] = NotifySub{
                            chars[c].char_handle, cccd_handle, ref_handle,
                            cu16, svc_u16, 0, 0, false, is_srv_chg
                        };
                        esp_ble_gattc_register_for_notify(gattc_if, s_remote_bda, chars[c].char_handle);
                    }
                }
            }
            advance_gattc_setup(gattc_if, param->search_cmpl.conn_id);
            break;
        }

        case ESP_GATTC_READ_DESCR_EVT:
            if (param->read.status == ESP_GATT_OK && param->read.value_len >= 2) {
                for (int i = 0; i < s_sub_count; ++i) {
                    if (s_subs[i].descr_ref_handle == param->read.handle) {
                        s_subs[i].report_id   = param->read.value[0];
                        s_subs[i].report_type = param->read.value[1];
                        ESP_LOGI(TAG, "  ReportRef char=0x%04x -> id=%u type=%u",
                                 s_subs[i].char_handle, s_subs[i].report_id, s_subs[i].report_type);
                    }
                }
            }
            advance_gattc_setup(gattc_if, param->read.conn_id);
            break;

        case ESP_GATTC_READ_CHAR_EVT:
            if (param->read.status == ESP_GATT_OK) {
                if (param->read.handle == s_dev_name_handle && param->read.value_len > 0) {
                    portENTER_CRITICAL(&s_state_mux);
                    const size_t cp = (param->read.value_len < sizeof(s_dev_name) - 1)
                                          ? param->read.value_len
                                          : (sizeof(s_dev_name) - 1);
                    memcpy(s_dev_name, param->read.value, cp);
                    s_dev_name[cp] = '\0';
                    portEXIT_CRITICAL(&s_state_mux);
                    ESP_LOGI(TAG, "  GAP Name: '%s'", s_dev_name);
                } else if (param->read.handle == s_appearance_handle && param->read.value_len >= 2) {
                    s_peer_appearance = param->read.value[0] | (param->read.value[1] << 8);
                    ESP_LOGI(TAG, "  GAP Appearance: 0x%04x", s_peer_appearance);
                } else if (param->read.handle == s_ppcp_handle && param->read.value_len >= 8) {
                    const uint8_t* v     = param->read.value;
                    const uint16_t min_i = v[0] | (v[1] << 8);
                    const uint16_t max_i = v[2] | (v[3] << 8);
                    const uint16_t lat   = v[4] | (v[5] << 8);
                    const uint16_t tout  = v[6] | (v[7] << 8);
                    ESP_LOGI(TAG, "  PPCP: min=%u max=%u lat=%u timeout=%u", min_i, max_i, lat, tout);
                    esp_ble_conn_update_params_t cp = {};
                    memcpy(cp.bda, s_remote_bda, 6);
                    cp.min_int = (min_i >= 6 && min_i <= 3200) ? min_i : 12;
                    cp.max_int = (max_i >= cp.min_int && max_i <= 3200) ? max_i : 24;
                    cp.latency = (lat <= 32) ? lat : 4;
                    cp.timeout = (tout >= 100 && tout <= 3200) ? tout : 500;
                    esp_ble_gap_update_conn_params(&cp);
                } else if (param->read.handle == s_battery_handle && param->read.value_len >= 1) {
                    portENTER_CRITICAL(&s_state_mux);
                    s_battery_pct = static_cast<int8_t>(param->read.value[0]);
                    portEXIT_CRITICAL(&s_state_mux);
                    ESP_LOGI(TAG, "  Battery: %d%%", s_battery_pct);
                } else if (param->read.handle == s_report_map_handle) {
                    ESP_LOGI(TAG, "  HID Report Map len=%u", param->read.value_len);
                    parse_hid_report_map(param->read.value, param->read.value_len);
                }
            }
            advance_gattc_setup(gattc_if, param->read.conn_id);
            break;

        case ESP_GATTC_WRITE_DESCR_EVT:
            advance_gattc_setup(gattc_if, param->write.conn_id);
            break;

        case ESP_GATTC_NOTIFY_EVT:
            for (int i = 0; i < s_sub_count; ++i) {
                if (s_subs[i].char_handle == param->notify.handle) {
                    const NotifySub& sub = s_subs[i];
                    if (sub.svc_uuid16 != ESP_GATT_UUID_HID_SVC ||
                        (sub.report_type != 1 && sub.report_type != 0)) {
                        break;
                    }
                    if (sub.uuid16 == ESP_GATT_UUID_HID_BT_KB_INPUT) {
                        parse_keyboard_report(param->notify.value, param->notify.value_len);
                        break;
                    }
                    const ReportLayout* layout = find_layout_by_id(sub.report_id);
                    ReportKind kind = layout ? layout->kind : REPORT_KIND_UNKNOWN;
                    if (kind == REPORT_KIND_UNKNOWN) {
                        if (s_peer_appearance == 0x03C3 || s_peer_appearance == 0x03C4) {
                            kind = REPORT_KIND_GAMEPAD;
                        } else if (sub.report_id <= 1) {
                            kind = REPORT_KIND_KEYBOARD;
                        } else {
                            kind = REPORT_KIND_OTHER;
                        }
                    }
                    if (kind == REPORT_KIND_KEYBOARD) {
                        parse_keyboard_report(param->notify.value, param->notify.value_len);
                    } else if (kind == REPORT_KIND_GAMEPAD) {
                        parse_gamepad_report(layout, param->notify.value, param->notify.value_len);
                    }
                    break;
                }
            }
            break;

        case ESP_GATTC_CLOSE_EVT:
            ESP_LOGI(TAG, "GATTC CLOSE reason=0x%x status=0x%x",
                     param->close.reason, param->close.status);
            portENTER_CRITICAL(&s_state_mux);
            s_conn_id          = 0xFFFF;
            s_connected        = false;
            s_connecting       = false;
            s_encrypted        = false;
            s_held_kbd_buttons = 0;
            s_held_pad_buttons = 0;
            s_held_buttons     = 0;
            s_last_keycode     = 0;
            s_battery_pct      = -1;
            s_peer_appearance  = 0;
            s_dev_name[0]      = '\0';
            portEXIT_CRITICAL(&s_state_mux);
            break;

        default:
            break;
    }
}

void perform_clear_bonds() {
    uint16_t cid      = 0xFFFF;
    esp_gatt_if_t gif = ESP_GATT_IF_NONE;
    portENTER_CRITICAL(&s_state_mux);
    cid = s_conn_id;
    gif = s_gattc_if;
    portEXIT_CRITICAL(&s_state_mux);

    if (cid != 0xFFFF && gif != ESP_GATT_IF_NONE) {
        esp_ble_gattc_close(gif, cid);
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
    static esp_ble_ext_scan_params_t ext_scan_params = {
        .own_addr_type  = BLE_ADDR_TYPE_PUBLIC,
        .filter_policy  = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
        .cfg_mask       = ESP_BLE_GAP_EXT_SCAN_CFG_UNCODE_MASK | ESP_BLE_GAP_EXT_SCAN_CFG_CODE_MASK,
        .uncoded_cfg    = {BLE_SCAN_TYPE_ACTIVE, 0x50, 0x50},
        .coded_cfg      = {BLE_SCAN_TYPE_ACTIVE, 0x50, 0x50},
    };

    xSemaphoreTake(s_scan_sem, 0);
    const esp_err_t sp_err = esp_ble_gap_set_ext_scan_params(&ext_scan_params);
    if (sp_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_set_ext_scan_params failed: %s", esp_err_to_name(sp_err));
    } else {
        xSemaphoreTake(s_scan_sem, pdMS_TO_TICKS(1500));
    }

    while (true) {
        bool     do_clear   = false;
        bool     connected  = false;
        bool     connecting = false;
        uint32_t start_ms   = 0;
        portENTER_CRITICAL(&s_state_mux);
        do_clear    = s_clear_req;
        s_clear_req = false;
        connected   = s_connected;
        connecting  = s_connecting;
        start_ms    = s_connect_start_ms;
        portEXIT_CRITICAL(&s_state_mux);

        if (do_clear) {
            perform_clear_bonds();
            continue;
        }

        if (connected) {
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        if (connecting) {
            const uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if ((now_ms - start_ms) > 10000) {
                ESP_LOGW(TAG, "BLE connect timeout, closing");
                if (s_conn_id != 0xFFFF && s_gattc_if != ESP_GATT_IF_NONE) {
                    esp_ble_gattc_close(s_gattc_if, s_conn_id);
                }
                portENTER_CRITICAL(&s_state_mux);
                s_connecting  = false;
                s_conn_id     = 0xFFFF;
                s_dev_name[0] = '\0';
                portEXIT_CRITICAL(&s_state_mux);
            } else {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            continue;
        }

        s_candidate  = CandidateDevice{};
        s_seen_count = 0;
        xSemaphoreTake(s_scan_sem, 0);

        portENTER_CRITICAL(&s_state_mux);
        s_scanning = true;
        portEXIT_CRITICAL(&s_state_mux);

        const esp_err_t sc_err = esp_ble_gap_start_ext_scan(300, 0);
        if (sc_err != ESP_OK) {
            ESP_LOGW(TAG, "esp_ble_gap_start_ext_scan failed: %s", esp_err_to_name(sc_err));
            portENTER_CRITICAL(&s_state_mux);
            s_scanning = false;
            portEXIT_CRITICAL(&s_state_mux);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        xSemaphoreTake(s_scan_sem, pdMS_TO_TICKS(3300));
        esp_ble_gap_stop_ext_scan();

        portENTER_CRITICAL(&s_state_mux);
        s_scanning = false;
        portEXIT_CRITICAL(&s_state_mux);

        if (s_candidate.valid && s_gattc_if != ESP_GATT_IF_NONE) {
            ESP_LOGI(TAG, "Connecting to '%s' (ext=%d)...", s_candidate.name, s_candidate.is_ext);
            portENTER_CRITICAL(&s_state_mux);
            s_connecting       = true;
            s_connect_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            s_peer_appearance  = s_candidate.appearance;
            snprintf(s_dev_name, sizeof(s_dev_name), "%s", s_candidate.name);
            portEXIT_CRITICAL(&s_state_mux);

            const esp_err_t oerr = s_candidate.is_ext
                ? esp_ble_gattc_aux_open(s_gattc_if, s_candidate.bda, s_candidate.addr_type, true)
                : esp_ble_gattc_open(s_gattc_if, s_candidate.bda, s_candidate.addr_type, true);
            if (oerr != ESP_OK) {
                ESP_LOGW(TAG, "esp_ble_gattc_open failed: %s", esp_err_to_name(oerr));
                portENTER_CRITICAL(&s_state_mux);
                s_connecting  = false;
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

    esp_ble_gatt_set_local_mtu(247);

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
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(ble_gattc_cb));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(0));

    s_initialized = true;
    xTaskCreatePinnedToCore(ble_hid_task, "ble_hid_task", 4096, nullptr, 3, nullptr, 0);
    ESP_LOGI(TAG, "BLE HID Host initialized (Keyboard + Gamepad HOGP 0x1812)");
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
