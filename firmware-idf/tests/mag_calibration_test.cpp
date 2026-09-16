#include "mag_calibration.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr double kPi = 3.14159265358979323846;

void expect(bool value, const char* reason) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}

float length(const float value[3]) {
    return std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
}

float angle(const float a[3], const float b[3]) {
    float cosine = (a[0]*b[0] + a[1]*b[1] + a[2]*b[2]) / (length(a) * length(b));
    cosine = std::max(-1.0f, std::min(1.0f, cosine));
    return std::acos(cosine) * 180.0f / static_cast<float>(kPi);
}

void fill(int16_t points[][3], int count, const float bias[3], const float distortion[3][3], float field,
          float zMin = -1.0f, float zMax = 1.0f, float noise = 0.0f) {
    const double golden = kPi * (3.0 - std::sqrt(5.0));
    for (int i = 0; i < count; ++i) {
        const float z = zMin + (zMax - zMin) * (i + 0.5f) / count;
        const float radius = std::sqrt(std::max(0.0f, 1.0f - z*z));
        const float truth[3] = {static_cast<float>(field * radius * std::cos(golden * i)),
                                static_cast<float>(field * radius * std::sin(golden * i)), field * z};
        for (int row = 0; row < 3; ++row) {
            float raw = bias[row];
            for (int column = 0; column < 3; ++column) raw += distortion[row][column] * truth[column];
            raw += noise * std::sin(17.0 * i + 3.0 * row);
            points[i][row] = static_cast<int16_t>(std::lround(raw * 10.0f));
        }
    }
}

}  // namespace

int main() {
    constexpr int count = 128;
    int16_t points[count][3];
    const float identity[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
    const float bias[3] = {18.0f, -12.0f, 7.0f};
    magcal2::Calibration calibration;
    fill(points, count, bias, identity, 49.0f);
    auto verdict = magcal2::fitAndJudge(points, count, &calibration);
    expect(verdict == magcal2::Verdict::Ok, "hard iron sphere accepted");
    expect(std::fabs(calibration.bias[0]-bias[0]) < .15f && std::fabs(calibration.bias[1]-bias[1]) < .15f &&
           std::fabs(calibration.bias[2]-bias[2]) < .15f, "hard iron centre recovered");
    expect(calibration.rmsUt < .1f, "sphere residual small");

    const float distorted[3][3] = {{1.30f, .16f, -.05f}, {.16f, .82f, .07f}, {-.05f, .07f, 1.02f}};
    fill(points, count, bias, distorted, 49.0f, -1.0f, 1.0f, .15f);
    verdict = magcal2::fitAndJudge(points, count, &calibration);
    if (verdict != magcal2::Verdict::Ok) std::fprintf(stderr, "verdict %s field %.2f rms %.2f cond %.2f coverage %.2f\n",
        magcal2::verdictText(verdict), calibration.fieldUt, calibration.rmsUt, calibration.condition, calibration.coverage);
    expect(verdict == magcal2::Verdict::Ok, "soft iron ellipsoid accepted");
    float worstAngle = 0.0f, minNorm = 1e9f, maxNorm = 0.0f;
    const double golden = kPi * (3.0 - std::sqrt(5.0));
    for (int i = 0; i < count; ++i) {
        const float z = -1.0f + 2.0f * (i + 0.5f) / count;
        const float radius = std::sqrt(1.0f-z*z);
        const float truth[3] = {static_cast<float>(49.0f*radius*std::cos(golden*i)),
                                static_cast<float>(49.0f*radius*std::sin(golden*i)),49.0f*z};
        const float raw[3] = {points[i][0]*.1f,points[i][1]*.1f,points[i][2]*.1f};
        float corrected[3]; magcal2::apply(calibration, raw, corrected);
        worstAngle = std::max(worstAngle, angle(corrected, truth));
        minNorm = std::min(minNorm, length(corrected)); maxNorm = std::max(maxNorm, length(corrected));
    }
    std::printf("soft iron: bias %.2f %.2f %.2f, field %.2f, rms %.3f, cond %.2f, coverage %.2f, angle %.2f, norm %.2f..%.2f\n",
        calibration.bias[0],calibration.bias[1],calibration.bias[2],calibration.fieldUt,calibration.rmsUt,
        calibration.condition,calibration.coverage,worstAngle,minNorm,maxNorm);
    expect(worstAngle < 1.0f && maxNorm-minNorm < 1.0f, "soft iron direction and magnitude corrected");

    fill(points, count, bias, identity, 49.0f, .75f, 1.0f);
    verdict = magcal2::fitAndJudge(points, count, &calibration);
    std::printf("cap: %s coverage %.3f field %.2f cond %.2f rms %.2f\n", magcal2::verdictText(verdict),
                calibration.coverage, calibration.fieldUt, calibration.condition, calibration.rmsUt);
    expect(verdict != magcal2::Verdict::Ok,
           "one-sided cap rejected");
    fill(points, count, bias, identity, 49.0f, -.02f, .02f);
    expect(magcal2::fitAndJudge(points, count, &calibration) != magcal2::Verdict::Ok,
           "flat circle rejected");
    expect(magcal2::fitAndJudge(points, 40, &calibration) == magcal2::Verdict::TooFew,
           "too few points rejected");
    const float extreme[3][3] = {{3.0f,0,0},{0,.5f,0},{0,0,1.0f}};
    fill(points,count,bias,extreme,49.0f);
    expect(magcal2::fitAndJudge(points,count,&calibration) != magcal2::Verdict::Ok,
           "extreme distortion rejected");
    std::puts("PASS magnetometer 3D hard/soft-iron calibration");
}
