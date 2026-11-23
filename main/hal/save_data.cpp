#include "hal/save_data.hpp"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "save_data";
static const char* NVS_NS = "gconsole";

namespace {

ConsoleSaveData s_data;
bool            s_nvs_ready = false;

void save_u8(const char* key, uint8_t val) {
    if (!s_nvs_ready) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

void save_u32(const char* key, uint32_t val) {
    if (!s_nvs_ready) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

void save_i16(const char* key, int16_t val) {
    if (!s_nvs_ready) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i16(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

} // namespace

void save_data_init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS partition due to %s", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS init failed (%s), using defaults", esp_err_to_name(err));
        return;
    }

    s_nvs_ready = true;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "vol", &s_data.volume_pct);
        nvs_get_u8(h, "bri", &s_data.brightness_pct);
        nvs_get_u32(h, "act_hi", &s_data.action_best_score);
        nvs_get_u32(h, "rhy0", &s_data.rhythm_best[0]);
        nvs_get_u32(h, "rhy1", &s_data.rhythm_best[1]);
        nvs_get_u32(h, "rhy2", &s_data.rhythm_best[2]);
        nvs_get_i16(h, "rhy_ofs", &s_data.rhythm_offset_ms);
        nvs_close(h);
    }
}

const ConsoleSaveData& save_data_get() {
    return s_data;
}

void save_data_set_volume(uint8_t volume_pct) {
    if (volume_pct > 100) volume_pct = 100;
    s_data.volume_pct = volume_pct;
    save_u8("vol", volume_pct);
}

void save_data_set_brightness(uint8_t brightness_pct) {
    if (brightness_pct > 100) brightness_pct = 100;
    s_data.brightness_pct = brightness_pct;
    save_u8("bri", brightness_pct);
}

void save_data_update_action_best(uint32_t score) {
    if (score > s_data.action_best_score) {
        s_data.action_best_score = score;
        save_u32("act_hi", score);
    }
}

void save_data_update_rhythm_best(uint8_t song_idx, uint32_t score) {
    if (song_idx >= 3) return;
    if (score > s_data.rhythm_best[song_idx]) {
        s_data.rhythm_best[song_idx] = score;
        const char* keys[3] = {"rhy0", "rhy1", "rhy2"};
        save_u32(keys[song_idx], score);
    }
}

void save_data_set_rhythm_offset(int16_t offset_ms) {
    if (offset_ms < -100) offset_ms = -100;
    if (offset_ms > 100)  offset_ms = 100;
    s_data.rhythm_offset_ms = offset_ms;
    save_i16("rhy_ofs", offset_ms);
}
