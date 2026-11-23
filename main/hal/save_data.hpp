#pragma once

#include <stdint.h>

struct ConsoleSaveData {
    uint8_t  volume_pct        = 65;
    uint8_t  brightness_pct    = 70;
    uint32_t action_best_score = 0;
    uint32_t rhythm_best[3]    = {0, 0, 0};
    int16_t  rhythm_offset_ms  = 16;
};

/**
 * @brief Initialize NVS flash and load persisted settings/high scores.
 */
void save_data_init();

/**
 * @brief Access current in-memory save data.
 */
const ConsoleSaveData& save_data_get();

/**
 * @brief Update and persist settings / high scores.
 */
void save_data_set_volume(uint8_t volume_pct);
void save_data_set_brightness(uint8_t brightness_pct);
void save_data_update_action_best(uint32_t score);
void save_data_update_rhythm_best(uint8_t song_idx, uint32_t score);
void save_data_set_rhythm_offset(int16_t offset_ms);
