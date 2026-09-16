#pragma once

#include "heading_tilt.h"
extern "C" {
#include "FusionAhrs.h"
}

namespace heading {

// Formula 5: 3D hard/soft-iron calibration + Fusion v1.3.3 NED, 100 Hz,
// gain .5, 10° rejection, 5 s recovery. HLG NAV stores raw magnetometer data.
// Change the formula ID if these semantics change. No GPS/COG input.
constexpr uint8_t kFormula = 5;
constexpr uint8_t kLogCaution = 0x08; // NAV event bit 3; Fusion heading quality caution
constexpr float kDisplayDampingHalfLifeSec = 1.0f; // PNI TCM2-style circular IIR
struct Reading {
    float degrees = -1.0f;
    float rawDegrees = -1.0f; // Fusion output before display damping; diagnostics/tests only
    bool caution = true;
    bool accelIgnored = true, magIgnored = true, recovering = false;
    float accelError = 0.0f, magError = 0.0f;
};

class Filter {
public:
    void reset();
    // Called once per FIFO sample. acc/gyro are MPU axes (g, deg/s), mag is
    // calibrated AK8963 axes (µT). nowMs is receipt time, tickMs the FIFO clock.
    // revision changes on reattach or calibration, not on recording start.
    void update(uint32_t tickMs, uint32_t nowMs, uint32_t revision,
                const hdg::HeadingCfg& cfg, const float acc[3], const float gyro[3],
                const float mag[3], uint32_t magAgeMs);
    Reading latest(uint32_t nowMs) const;
private:
    FusionAhrs ahrs_{};
    hdg::HeadingCfg cfg_{};
    Reading reading_{};
    uint32_t tickMs_ = 0, receivedMs_ = 0, magMs_ = 0, revision_ = 0;
    bool started_ = false;
};
} // namespace heading
