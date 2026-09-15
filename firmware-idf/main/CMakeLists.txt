# 아두이노 흔적이 없는 헤더는 firmware-rak/include 를 **복사하지 않고 같이 쓴다** (PORTING.md 규칙 4).
idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "." "../../firmware-rak/include"
    REQUIRES nvs_flash esp_driver_gpio esp_driver_uart esp_driver_i2c esp_adc esp_psram spi_flash esp_timer
             fatfs esp_driver_spi esp_driver_sdspi sdmmc
)
