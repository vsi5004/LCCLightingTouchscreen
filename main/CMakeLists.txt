idf_component_register(
    SRCS 
        "main.c"
        "app/scene_storage.c"
        "app/lcc_node.cpp"
        "app/fade_controller.c"
        "app/screen_timeout.c"
        "app/bootloader_hal.cpp"
        "app/bootloader_display.c"
        "ui/ui_common.c"
        "ui/ui_main.c"
        "ui/ui_manual.c"
        "ui/ui_scenes.c"
    INCLUDE_DIRS 
        "."
        "app"
        "ui"
    REQUIRES
        driver
        esp_lcd
        esp_timer
        fatfs
        nvs_flash
        board_drivers
        espressif__esp_jpeg
        lvgl__lvgl
        json
        OpenMRN
        app_update
)

# Set C++ standard for OpenMRN compatibility
set_source_files_properties(
    "app/lcc_node.cpp"
    "app/bootloader_hal.cpp"
    PROPERTIES COMPILE_FLAGS "-std=gnu++14 -fno-strict-aliasing"
)
