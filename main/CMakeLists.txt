idf_component_register(
    SRCS
        "main.cpp"
        "hal/display.cpp"
        "hal/input.cpp"
        "hal/ble_hid.cpp"
        "hal/audio.cpp"
        "hal/save_data.cpp"
        "game/launcher_game.cpp"
        "game/hw_test_game.cpp"
        "game/action_game.cpp"
        "game/rhythm_game.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        driver
        esp_timer
        esp_psram
        nvs_flash
        bt
        esp_hid
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
