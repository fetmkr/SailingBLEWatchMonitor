// 비밀 값 틀. 이 파일은 커밋되지만 진짜 값은 들어가지 않는다.
//
// 쓰는 법
//   cp include/secrets.example.h include/secrets.h
//   secrets.h 에 진짜 값을 적는다.
//
// secrets.h 는 .gitignore 에 들어 있어서 커밋되지 않는다.
// 확인하려면:  git check-ignore -v firmware-rak/include/secrets.h
#pragma once

// 접속할 WiFi. 쓰지 않으면 빈 문자열로 둔다.
#define SAIL_WIFI_SSID ""
#define SAIL_WIFI_PASS ""
