idf_component_register(
    SRCS
        "main.cpp"
        "hal/display.cpp"
        "hal/input.cpp"
        "hal/audio.cpp"
        "game/hw_test_game.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        driver
        esp_timer
        esp_psram
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
