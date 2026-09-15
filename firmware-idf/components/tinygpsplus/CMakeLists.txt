# TinyGPS++ 1.1.0 (Mikal Hart, LGPL 2.1 — LICENSE) 를 firmware-rak/.pio/libdeps/rak3112/TinyGPSPlus 에서 복사했다.
# 바꾼 곳은 src/TinyGPS++.h · src/TinyGPS++.cpp 맨 위에 적었다.
idf_component_register(
    SRCS "src/TinyGPS++.cpp"
    INCLUDE_DIRS "src"
    PRIV_REQUIRES esp_timer
)
# encode() 의 case ',' 가 '\r' '\n' '*' 로 일부러 흘러내린다 (쉼표도 항목 끝이다 — TinyGPS++.cpp 84-88).
# IDF 는 이걸 오류로 막는다. 원본을 안 고치고 이 부품에서만 끈다.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-implicit-fallthrough)
