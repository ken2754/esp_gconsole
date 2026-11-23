#pragma once

#include <stdint.h>

struct BleHidStatus {
    bool    initialized  = false;
    bool    scanning     = false;
    bool    connected    = false;
    int8_t  battery_pct  = -1;
    uint8_t last_keycode = 0;
    char    device_name[32] = {};
};

/**
 * @brief Initialize BLE controller, Bluedroid stack, and ESP HID Host
 *        to connect to an 8BitDo Micro (K-mode) BLE keyboard controller.
 *        Requires nvs_flash_init() (via save_data_init()) to have been called first.
 */
bool ble_hid_init();

/**
 * @brief Returns current bitmask of held ButtonMask flags from BLE HID keyboard input.
 */
uint16_t ble_hid_get_buttons();

/**
 * @brief Returns current BLE HID connection and diagnostic status.
 */
BleHidStatus ble_hid_get_status();

/**
 * @brief Disconnect current HID device, erase bonded BLE peers, and start fresh scan.
 */
void ble_hid_clear_bonds();
