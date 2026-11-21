// 音量の計測（20ms フレームごと）
#pragma once

#include <cstddef>
#include <cstdint>

struct FrameLevel {
    float rms_db;   // dBFS
    float peak_db;  // dBFS
    bool clipped;
};

FrameLevel measure_frame(const int16_t* samples, size_t n);
