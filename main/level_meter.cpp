#include "level_meter.h"

#include <cmath>

namespace {
constexpr float kFloorDb = -90.0f;
constexpr int kClipThreshold = 32500;

float to_db(float linear) {
    if (linear <= 0.0f) return kFloorDb;
    float db = 20.0f * log10f(linear / 32768.0f);
    return db < kFloorDb ? kFloorDb : db;
}
}  // namespace

FrameLevel measure_frame(const int16_t* s, size_t n) {
    FrameLevel lv{kFloorDb, kFloorDb, false};
    if (n == 0) return lv;
    int64_t sum_sq = 0;
    int peak = 0;
    for (size_t i = 0; i < n; i++) {
        int v = s[i];
        sum_sq += static_cast<int64_t>(v) * v;
        int a = v < 0 ? -v : v;
        if (a > peak) peak = a;
    }
    lv.rms_db = to_db(sqrtf(static_cast<float>(sum_sq) / n));
    lv.peak_db = to_db(static_cast<float>(peak));
    lv.clipped = peak >= kClipThreshold;
    return lv;
}
