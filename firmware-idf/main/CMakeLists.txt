# 아두이노 흔적이 없는 헤더는 firmware-rak/include 를 **복사하지 않고 같이 쓴다** (PORTING.md 규칙 4).
# ★ SRC_DIRS "." — 이 폴더의 .c/.cpp 를 전부 짓는다. 여럿이 나눠 짤 때 이 파일을 안 건드리게 하려고 (PORTING.md "작업 나누기").
#   파일을 새로 더하면 CMake 가 다시 훑어야 한다: idf.py -B ~/esp/build-sail reconfigure
idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "." "../../firmware-rak/include"
    REQUIRES nvs_flash esp_driver_gpio esp_driver_uart esp_driver_i2c esp_adc esp_psram spi_flash esp_timer
             fatfs esp_driver_spi esp_driver_sdspi sdmmc mbedtls esp_driver_usb_serial_jtag tinygpsplus
)
