# U8g2 2.36.18 (olikraus, BSD-2-Clause) 의 C 원본 — firmware-rak 이 PlatformIO 로 받은 것과 같은 판.
#   가져온 곳: firmware-rak/.pio/libdeps/rak3112/U8g2/src/clib
#   뺀 것: u8g2_fonts.c (39 MB) · u8x8_fonts.c · mui* · u8log* — 화면이 안 쓴다
#   더한 것: clib/u8g2_fonts_sail.c — 쓰는 글꼴 셋(5x7_tf · 6x10_tf · unifont_t_korean2)만 원본에서 그대로 떼어 옴
# 아두이노 C++ 겉옷(U8g2lib.cpp)과 아두이노 I2C 콜백은 안 가져온다. I2C 는 main/display.cpp 가 i2c_master 로 한다.
file(GLOB U8G2_SRCS "${CMAKE_CURRENT_LIST_DIR}/clib/*.c")
idf_component_register(SRCS ${U8G2_SRCS} INCLUDE_DIRS "clib")
# 남의 라이브러리 원본은 고치지 않는다. 경고만 이 부품에서 끈다 (우리 코드에는 안 걸린다).
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
