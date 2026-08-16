#pragma once

#include <math.h>
#include <stdint.h>

namespace RfRoute {

static constexpr uint8_t kCapacity = 48u;
static constexpr uint32_t kMaxFixAgeMs = 2500u;
static constexpr float kMaxHdop = 5.0f;
static constexpr float kMinCourseSpeedKmh = 5.0f;
static constexpr uint32_t kMinSampleSpacingMs = 1000u;

struct Fix {
    double latitude = 0.0;
    double longitude = 0.0;
    uint32_t ageMs = UINT32_MAX;
    float hdop = 0.0f;
    float speedKmh = 0.0f;
    float courseDeg = 0.0f;
    bool courseValid = false;
};

struct Sample {
    uint32_t timestampMs = 0u;
    int32_t latitudeE7 = 0;
    int32_t longitudeE7 = 0;
    int8_t rssi = -127;
    uint16_t fixAgeMs = UINT16_MAX;
    uint8_t hdopX10 = 0u;
    uint8_t speedKmh = 0u;
    uint16_t courseDegX10 = 0u;
    bool courseConstraintEligible = false;
};

struct StrongestRegion {
    int32_t latitudeE7 = 0;
    int32_t longitudeE7 = 0;
    uint16_t radiusMeters = 0u;
    int8_t strongestRssi = -127;
    uint8_t contributingSamples = 0u;
    bool valid = false;
};

inline bool fixUsable(const Fix& fix) {
    return fix.ageMs <= kMaxFixAgeMs &&
           fix.hdop > 0.0f && fix.hdop <= kMaxHdop &&
           fix.latitude >= -90.0 && fix.latitude <= 90.0 &&
           fix.longitude >= -180.0 && fix.longitude <= 180.0 &&
           !(fix.latitude == 0.0 && fix.longitude == 0.0);
}

inline int32_t degreesToE7(double value) {
    const double scaled = value * 10000000.0;
    return static_cast<int32_t>(
        scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
}

inline double e7ToDegrees(int32_t value) {
    return static_cast<double>(value) / 10000000.0;
}

inline float distanceMeters(int32_t latAE7, int32_t lonAE7,
                            int32_t latBE7, int32_t lonBE7) {
    static constexpr double kEarthRadiusMeters = 6371000.0;
    static constexpr double kDegToRad =
        0.017453292519943295769236907684886;
    const double latA = e7ToDegrees(latAE7) * kDegToRad;
    const double latB = e7ToDegrees(latBE7) * kDegToRad;
    const double dLat = latB - latA;
    const double dLon =
        (e7ToDegrees(lonBE7) - e7ToDegrees(lonAE7)) * kDegToRad;
    const double sinLat = sin(dLat * 0.5);
    const double sinLon = sin(dLon * 0.5);
    const double a = sinLat * sinLat +
                     cos(latA) * cos(latB) * sinLon * sinLon;
    const double bounded = a > 1.0 ? 1.0 : (a < 0.0 ? 0.0 : a);
    return static_cast<float>(
        2.0 * kEarthRadiusMeters * asin(sqrt(bounded)));
}

class Tracker {
public:
    void reset() {
        for (Sample& sample : samples_) {
            sample = {};
        }
        head_ = 0u;
        count_ = 0u;
        lastAcceptedMs_ = 0u;
    }

    bool observe(uint32_t timestampMs, int8_t rssi, const Fix& fix) {
        if (!fixUsable(fix)) return false;
        if (lastAcceptedMs_ != 0u &&
            timestampMs - lastAcceptedMs_ < kMinSampleSpacingMs) {
            return false;
        }

        Sample sample{};
        sample.timestampMs = timestampMs;
        sample.latitudeE7 = degreesToE7(fix.latitude);
        sample.longitudeE7 = degreesToE7(fix.longitude);
        sample.rssi = rssi;
        sample.fixAgeMs = static_cast<uint16_t>(
            fix.ageMs > UINT16_MAX ? UINT16_MAX : fix.ageMs);
        const int hdopX10 = static_cast<int>(fix.hdop * 10.0f + 0.5f);
        sample.hdopX10 = static_cast<uint8_t>(
            hdopX10 > 255 ? 255 : (hdopX10 < 0 ? 0 : hdopX10));
        const int speed = static_cast<int>(fix.speedKmh + 0.5f);
        sample.speedKmh = static_cast<uint8_t>(
            speed > 255 ? 255 : (speed < 0 ? 0 : speed));
        int courseX10 =
            static_cast<int>(fix.courseDeg * 10.0f + 0.5f);
        while (courseX10 >= 3600) courseX10 -= 3600;
        while (courseX10 < 0) courseX10 += 3600;
        sample.courseDegX10 = static_cast<uint16_t>(courseX10);
        // This only marks when course could become a slow drift constraint.
        // The firmware deliberately does not apply it until device-versus-
        // travel orientation is known.
        sample.courseConstraintEligible =
            fix.courseValid && fix.speedKmh >= kMinCourseSpeedKmh;

        samples_[head_] = sample;
        head_ = static_cast<uint8_t>((head_ + 1u) % kCapacity);
        if (count_ < kCapacity) ++count_;
        lastAcceptedMs_ = timestampMs == 0u ? 1u : timestampMs;
        return true;
    }

    uint8_t count() const { return count_; }

    bool newest(Sample& out) const {
        if (count_ == 0u) return false;
        const uint8_t index =
            static_cast<uint8_t>((head_ + kCapacity - 1u) % kCapacity);
        out = samples_[index];
        return true;
    }

    StrongestRegion strongestRegion() const {
        StrongestRegion out{};
        if (count_ < 3u) return out;

        uint8_t ordered[kCapacity] = {};
        const uint8_t oldest = static_cast<uint8_t>(
            (head_ + kCapacity - count_) % kCapacity);
        for (uint8_t i = 0u; i < count_; ++i) {
            ordered[i] = static_cast<uint8_t>((oldest + i) % kCapacity);
        }
        for (uint8_t i = 1u; i < count_; ++i) {
            const uint8_t key = ordered[i];
            uint8_t j = i;
            while (j > 0u &&
                   samples_[ordered[j - 1u]].rssi < samples_[key].rssi) {
                ordered[j] = ordered[j - 1u];
                --j;
            }
            ordered[j] = key;
        }

        uint8_t contributors = static_cast<uint8_t>(count_ / 4u);
        if (contributors < 3u) contributors = 3u;
        if (contributors > 12u) contributors = 12u;
        if (contributors > count_) contributors = count_;

        int64_t latitudeSum = 0;
        int64_t longitudeSum = 0;
        for (uint8_t i = 0u; i < contributors; ++i) {
            latitudeSum += samples_[ordered[i]].latitudeE7;
            longitudeSum += samples_[ordered[i]].longitudeE7;
        }
        out.latitudeE7 =
            static_cast<int32_t>(latitudeSum / contributors);
        out.longitudeE7 =
            static_cast<int32_t>(longitudeSum / contributors);
        out.strongestRssi = samples_[ordered[0u]].rssi;
        out.contributingSamples = contributors;

        float radius = 0.0f;
        for (uint8_t i = 0u; i < contributors; ++i) {
            const Sample& sample = samples_[ordered[i]];
            const float distance = distanceMeters(
                out.latitudeE7, out.longitudeE7,
                sample.latitudeE7, sample.longitudeE7);
            if (distance > radius) radius = distance;
        }
        const uint32_t roundedRadius =
            static_cast<uint32_t>(radius + 0.5f);
        out.radiusMeters = static_cast<uint16_t>(
            roundedRadius > UINT16_MAX ? UINT16_MAX : roundedRadius);
        out.valid = true;
        return out;
    }

private:
    Sample samples_[kCapacity] = {};
    uint8_t head_ = 0u;
    uint8_t count_ = 0u;
    uint32_t lastAcceptedMs_ = 0u;
};

}  // namespace RfRoute
