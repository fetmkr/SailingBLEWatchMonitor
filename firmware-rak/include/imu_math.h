// IMU 단위 환산. 보드 없이 시험한다 (tools/fw_logic_test.cpp).
#pragma once

#include <cmath>

namespace imu {

// ── 자이로 0점 단위 ──────────────────────────────────────────────────────
//
// MPU9250_WE 1.2.17 의 correctGyrRawValues() 는 이렇게 뺀다
// [확인: .pio/libdeps/rak3112/MPU9250_WE/src/MPU6500_WE.cpp#L682-L686]
//
//     raw -= offset / gyrRangeFactor
//
// gyrRangeFactor 는 setGyrRange 가 (1 << range) 로 둔다 (#L248). ±1000°/s 면 4 다.
// 즉 **오프셋은 ±250°/s 범위의 원시값 단위**여야 한다.
//
// 옛 코드는 ±1000 범위에서 잰 원시 평균을 그대로 넣었다. 그러면 1/4 만 빠진다.
// 원시 영점 100 이면 25 만 빼고 75 가 남았다.
constexpr int kGyrRangeFactor1000 = 4;   // MPU9250_GYRO_RANGE_1000 = 2 → 1 << 2

// 지금 범위에서 잰 원시 평균 → 라이브러리가 받는 오프셋
inline float gyrOffsetForLib(float meanRawAtRange, int rangeFactor) {
    return meanRawAtRange * (float)rangeFactor;
}

// 라이브러리가 실제로 빼는 값을 흉내 낸다 (시험용)
inline float libCorrect(float rawAtRange, float offset, int rangeFactor) {
    return rawAtRange - offset / (float)rangeFactor;
}

// 지금 범위의 원시값 → °/s
inline float rawToDps(float rawAtRange, int rangeFactor) {
    return rawAtRange * (float)rangeFactor * 250.0f / 32768.0f;
}

// 라이브러리 오프셋(±250 범위 원시 단위) → °/s
inline float libOffsetToDps(float offset) { return offset * 250.0f / 32768.0f; }

// NVS 에 옛 단위(±1000 범위 원시값)로 적힌 0점을 새 단위로 옮긴다.
// 옛 값은 늘 imuBegin() 이 ±1000 으로 맞춘 뒤에 잰 것이다 (setup·calib 둘 다).
inline float migrateLegacyOffset(float legacyRawAt1000) {
    return legacyRawAt1000 * (float)kGyrRangeFactor1000;
}

// 0점으로 받아도 되나.
//   spreadDps  재는 동안 가장 크게 흔들린 폭 (°/s)
//   meanDps    평균 (°/s). 영점치고 너무 크면 돌고 있던 것이다
inline bool gyrCalAcceptable(float spreadDps, float meanAbsMaxDps,
                             float maxSpreadDps, float maxBiasDps) {
    if (!std::isfinite(spreadDps) || !std::isfinite(meanAbsMaxDps)) return false;
    return spreadDps <= maxSpreadDps && meanAbsMaxDps <= maxBiasDps;
}

} // namespace imu
