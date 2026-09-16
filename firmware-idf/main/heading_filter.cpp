#include "heading_filter.h"

namespace heading {
namespace {
constexpr uint32_t kMagFeedbackAgeMs = 250; // AK8963 8 Hz, polled at 10 Hz
constexpr uint32_t kMagCoastMs = 2000;      // short gyro-only estimate, marked ?
constexpr uint32_t kImuAgeMs = 250;

bool sameConfig(const hdg::HeadingCfg& a, const hdg::HeadingCfg& b) {
    return a.axisA == b.axisA && a.axisB == b.axisB && a.signA == b.signA &&
           a.signB == b.signB && a.offDeg == b.offDeg && a.declDeg == b.declDeg;
}
bool finite(FusionVector v) {
    return std::isfinite(v.axis.x) && std::isfinite(v.axis.y) && std::isfinite(v.axis.z);
}
FusionVector body(const float v[3], const hdg::HeadingCfg& cfg, bool mpu) {
    const float m[3] = {mpu ? v[1] : v[0], mpu ? v[0] : v[1], mpu ? -v[2] : v[2]};
    FusionVector b = FUSION_VECTOR_ZERO;
    hdg::toFRD(m, cfg, &b.axis.x, &b.axis.y, &b.axis.z);
    return b;
}
bool usableMag(FusionVector m, FusionVector gravity) {
    // Fusion normalises this cross product. A vertical/zero/NaN field has no heading.
    return finite(m) && FusionVectorNormSquared(m) > 1e-6f &&
           FusionVectorNormSquared(FusionVectorCross(gravity, m)) > 1e-6f;
}
FusionQuaternion initialQuaternion(float roll, float pitch, float yaw) {
    const float cr = cosf(roll/2), sr = sinf(roll/2);
    const float cp = cosf(pitch/2), sp = sinf(pitch/2);
    const float cy = cosf(yaw/2), sy = sinf(yaw/2);
    return {{cr*cp*cy + sr*sp*sy, sr*cp*cy - cr*sp*sy,
             cr*sp*cy + sr*cp*sy, cr*cp*sy - sr*sp*cy}};
}
} // namespace

void Filter::reset() {
    started_ = false;
    reading_ = {};
}

void Filter::update(uint32_t tickMs, uint32_t nowMs, uint32_t revision,
                    const hdg::HeadingCfg& cfg, const float acc[3], const float gyro[3],
                    const float mag[3], uint32_t magAgeMs) {
    if (hdg::downSign(cfg) == 0 || fabsf(cfg.signA) != 1 || fabsf(cfg.signB) != 1 ||
        !std::isfinite(cfg.offDeg) || !std::isfinite(cfg.declDeg)) { reset(); return; }
    if (started_ && (revision != revision_ || !sameConfig(cfg, cfg_))) reset();
    const uint32_t dtMs = tickMs - tickMs_;
    if (started_ && dtMs == 0) return; // repeated FIFO sample cannot advance state
    if (started_ && (dtMs < 5 || dtMs > 20)) reset(); // never integrate across lost samples/clock rebasing

    FusionVector a = body(acc, cfg, true), g = body(gyro, cfg, true), m = body(mag, cfg, false);
    if (!finite(g)) { reset(); return; }
    if (!finite(a) || FusionVectorNormSquared(a) < 1e-6f) a = FUSION_VECTOR_ZERO;
    const FusionVector gravity = started_ ? FusionAhrsGetGravity(&ahrs_) : a;
    const bool magUsable = magAgeMs <= kMagFeedbackAgeMs && usableMag(m, gravity);
    if (!magUsable) m = FUSION_VECTOR_ZERO;

    if (!started_) {
        float roll, pitch;
        if (!magUsable || !hdg::gravityRollPitch(acc, cfg, &roll, &pitch, false)) return;
        hdg::HeadingCfg zero = cfg; zero.offDeg = zero.declDeg = 0;
        const float yaw = hdg::tiltHeadingDeg(acc, mag, zero, false);
        if (yaw < 0) return;
        FusionAhrsInitialise(&ahrs_);
        const FusionAhrsSettings settings = {100.0f, FusionConventionNed, .5f, 1000.0f, 10.0f, 10.0f, 5.0f};
        FusionAhrsSetSettings(&ahrs_, &settings);
        FusionAhrsSetQuaternion(&ahrs_, initialQuaternion(roll, pitch, FusionDegreesToRadians(yaw)));
        cfg_ = cfg; revision_ = revision; started_ = true;
        // The seed already represents this sample; do not integrate it twice.
    } else {
        FusionAhrsSetSamplePeriod(&ahrs_, dtMs * .001f);
        // Latest magnetic sample held for <=250 ms; not presented as new 100 Hz measurements.
        // Update with zero mag, not UpdateNoMagnetometer (which zeros yaw during startup).
        FusionAhrsUpdate(&ahrs_, g, a, m);
    }
    tickMs_ = tickMs; receivedMs_ = nowMs;
    if (magUsable) magMs_ = nowMs - magAgeMs;
    const auto q = FusionAhrsGetQuaternion(&ahrs_);
    const float norm = FusionQuaternionNormSquared(q);
    if (!std::isfinite(norm) || norm < .5f) { reset(); return; }
    const auto states = FusionAhrsGetInternalStates(&ahrs_);
    const auto flags = FusionAhrsGetFlags(&ahrs_);
    const float rawDegrees = hdg::wrap360(FusionQuaternionToEuler(q).angle.yaw + cfg.offDeg + cfg.declDeg);
    reading_.rawDegrees = rawDegrees;
    if (reading_.degrees < 0.0f || !std::isfinite(reading_.degrees)) {
        reading_.degrees = rawDegrees;
    } else {
        // Old electronic compasses such as PNI TCM2 damped the displayed heading
        // with a one-pole IIR.  Filter the shortest circular error so 359/0 is continuous.
        const float keep = powf(0.5f, dtMs * 0.001f / kDisplayDampingHalfLifeSec);
        reading_.degrees = hdg::wrap360(reading_.degrees +
                           (1.0f - keep) * hdg::wrap180(rawDegrees - reading_.degrees));
    }
    reading_.accelIgnored = states.accelerometerIgnored;
    reading_.magIgnored = states.magnetometerIgnored || !magUsable;
    reading_.recovering = flags.startup || flags.overrangeRecovery || flags.accelerationRecovery || flags.magneticRecovery;
    reading_.accelError = states.accelerationError; reading_.magError = states.magneticError;
    // Translation and wave motion often make Fusion reject accelerometer feedback.
    // That is normal gyro propagation, not by itself a loss of heading reference.
    // Show ? only while the heading reference itself is weak or being recovered.
    reading_.caution = reading_.magIgnored || flags.startup ||
                       flags.overrangeRecovery || flags.magneticRecovery;
}

Reading Filter::latest(uint32_t nowMs) const {
    if (!started_ || nowMs - receivedMs_ > kImuAgeMs || nowMs - magMs_ > kMagCoastMs) return {};
    Reading out = reading_;
    out.caution |= nowMs - magMs_ > kMagFeedbackAgeMs;
    return out;
}
} // namespace heading
