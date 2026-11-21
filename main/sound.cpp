#include "sound.h"

#include <atomic>
#include <cstdint>

#include "M5Unified.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace sound {
namespace {

constexpr uint8_t kVolume = 112;
constexpr uint8_t kChannel = 0;  // 操作音はいつも同じチャンネルで鳴らす

// 矩形波（1 周期 16 点。再生時に点の間が直線で補間されるので、半分ずつ同じ値にして角を保つ）
constexpr uint8_t kSquareWave[16] = {255, 255, 255, 255, 255, 255, 255, 255, 1, 1, 1, 1, 1, 1, 1, 1};
constexpr uint16_t kNoteMs = 30;  // 1 音の長さ

// 音の高さ（Hz）
constexpr float kC5 = 523.25f;
constexpr float kE5 = 659.25f;
constexpr float kG5 = 783.99f;
constexpr float kC6 = 1046.50f;

struct Note {
    float hz;
    uint16_t ms;
};

constexpr Note kPowerOn[] = {{kC5, kNoteMs}, {kE5, kNoteMs}, {kG5, kNoteMs}};
constexpr Note kPowerOff[] = {{kG5, kNoteMs}, {kE5, kNoteMs}, {kC5, kNoteMs}};
constexpr Note kRecStart[] = {{kG5, kNoteMs}, {kC6, kNoteMs}};
constexpr Note kRecStop[] = {{kC6, kNoteMs}, {kG5, kNoteMs}};
// 同期信号は時報と同じ音程（予報音 A4、正報音 A5）
constexpr Note kSyncPip[] = {{440.0f, kSyncPipMs}};
constexpr Note kSyncMark[] = {{880.0f, kSyncMarkMs}};

QueueHandle_t s_queue = nullptr;
std::atomic<int> s_pending{0};  // 鳴らし終わっていない操作音の数

template <size_t N>
void play_notes(const Note (&notes)[N]) {
    for (const Note& n : notes) {
        M5.Speaker.tone(n.hz, n.ms, kChannel, true, kSquareWave, sizeof(kSquareWave));
        vTaskDelay(pdMS_TO_TICKS(n.ms));
    }
}

void sound_task(void*) {
    for (;;) {
        Cue cue;
        if (xQueueReceive(s_queue, &cue, portMAX_DELAY) != pdTRUE) continue;
        switch (cue) {
            case Cue::PowerOn: play_notes(kPowerOn); break;
            case Cue::PowerOff: play_notes(kPowerOff); break;
            case Cue::RecStart: play_notes(kRecStart); break;
            case Cue::RecStop: play_notes(kRecStop); break;
            case Cue::SyncPip: play_notes(kSyncPip); break;
            case Cue::SyncMark: play_notes(kSyncMark); break;
        }
        s_pending.fetch_sub(1);
    }
}

}  // namespace

// 録音処理（優先度 10）や画面の描画に押されると音が途切れる・最初の音が抜けるので、
// スピーカーのタスク（M5Unified）と、音を並べるタスクを、録音処理より上の優先度でコア 1 に置く
constexpr uint8_t kSpeakerTaskPriority = 12;
constexpr uint8_t kSoundTaskPriority = 11;
constexpr uint8_t kSoundCore = 1;

void init() {
    auto cfg = M5.Speaker.config();
    cfg.task_priority = kSpeakerTaskPriority;
    cfg.task_pinned_core = kSoundCore;
    M5.Speaker.config(cfg);
    M5.Speaker.begin();
    M5.Speaker.setVolume(kVolume);
    s_queue = xQueueCreate(4, sizeof(Cue));
    xTaskCreatePinnedToCore(sound_task, "sound", 3072, nullptr, kSoundTaskPriority, nullptr, kSoundCore);
}

void play(Cue cue) {
    if (!s_queue) return;
    // 音のタスクは優先度が高く、送った瞬間に鳴らし終えることがあるので、先に数を増やす
    s_pending.fetch_add(1);
    if (xQueueSend(s_queue, &cue, 0) != pdTRUE) s_pending.fetch_sub(1);
}

bool busy() { return s_pending.load() > 0; }

void set_loud(bool on) { M5.Speaker.setVolume(on ? 255 : kVolume); }

}  // namespace sound
