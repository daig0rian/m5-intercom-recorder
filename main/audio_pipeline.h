// audio タスク: PCM リングバッファ → 20ms フレーム → レベル計測 → Opus → Ogg → SD
#pragma once

#include <cstdint>

namespace audio_pipeline {

void start();

}  // namespace audio_pipeline
