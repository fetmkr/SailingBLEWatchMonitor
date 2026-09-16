#pragma once

#include <cstdint>

namespace magcal2 {

// x_corrected = matrix * (x_raw - bias).  The matrix is row-major.
// version 0: no calibration, 1: legacy hard-iron only, 2: hard + soft iron.
struct Calibration {
    uint8_t version = 0;
    float bias[3] = {0.0f, 0.0f, 0.0f};
    float matrix[9] = {1.0f, 0.0f, 0.0f,
                       0.0f, 1.0f, 0.0f,
                       0.0f, 0.0f, 1.0f};
    float fieldUt = 0.0f;
    float rmsUt = 0.0f;
    float condition = 1.0f;
    float coverage = 0.0f;
};

enum class Verdict : uint8_t {
    Ok,
    TooFew,
    Singular,
    NotEllipsoid,
    FieldOut,
    PoorCoverage,
    DistortionHigh,
    ResidualHigh,
};

constexpr int kMinPoints = 80;

Calibration identity();
void apply(const Calibration& calibration, const float raw[3], float corrected[3]);
bool usable(const Calibration& calibration);
Verdict fitAndJudge(const int16_t (*points)[3], int count, Calibration* result);
const char* verdictText(Verdict verdict);

}  // namespace magcal2
