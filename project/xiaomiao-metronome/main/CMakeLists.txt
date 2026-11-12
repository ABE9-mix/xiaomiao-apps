idf_component_register(SRCS "main.c" "ark12_font.c" "ark24_font.c"
                       PRIV_REQUIRES esp_lcd
                                     esp_timer
                                     esp_driver_gpio
                                     esp_driver_spi
                                     esp_driver_ledc
                                     esp_driver_i2c
                                     nvs_flash
                                     lvgl
                       INCLUDE_DIRS "")

target_link_libraries(${COMPONENT_LIB} PRIVATE m)
