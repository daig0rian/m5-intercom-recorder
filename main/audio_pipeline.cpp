#include "audio_pipeline.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "app_state.h"
#include "encoder/impl/esp_opus_enc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "level_meter.h"
#include "ogg_opus_writer.h"
#include "pcm_ring.h"
#include "rtc_clock.h"
#include "storage.h"

static const char* TAG = "audio";

namespace audio_pipeline {
namespace {

constexpr int kMaxFrameSamples = 16000 / 50;   // 20ms @ 16kHz
constexpr uint32_t kSamples48kPerFrame = 960;  // 20ms を 48kHz で数えた長さ
constexpr uint16_t kDefaultPreSkip = 312;      // Opus の標準的な先読み量（48kHz 換算）
constexpr int kComplexity = 3;
constexpr const char* kVendor = "m5-intercom-recorder";

struct Encoder {
    void* handle = nullptr;
    uint32_t rate = 0;
    int in_size = 0;
    int out_size = 0;
    uint16_t pre_skip = kDefaultPreSkip;
};

Encoder s_enc;
uint8_t* s_out = nullptr;
size_t s_out_cap = 0;

OggOpusWriter s_ogg;
bool s_file_open = false;
uint32_t s_file_rate = 0;
uint32_t s_file_frames = 0;

// 録音中に音声接続が閉じている間（途切れ）は、経った時間の分だけ無音を書き、
// 録音の時間を実際の時間に合わせる（ファイル名の日時から動画と合わせられるように）
bool s_gap = false;
int64_t s_gap_start_us = 0;
uint32_t s_gap_frames = 0;

void close_encoder() {
    if (s_enc.handle) esp_opus_enc_close(s_enc.handle);
    s_enc = Encoder{};
}

bool open_encoder(uint32_t rate) {
    close_encoder();
    esp_opus_enc_config_t cfg = {};
    cfg.sample_rate = static_cast<int>(rate);
    cfg.channel = 1;
    cfg.bits_per_sample = 16;
    cfg.bitrate = rate >= 16000 ? 20000 : 12000;
    cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    cfg.complexity = kComplexity;
    cfg.enable_fec = false;
    cfg.enable_dtx = false;  // 無音でもフレームを出し、時間軸を保つ
    cfg.enable_vbr = true;

    if (esp_opus_enc_open(&cfg, sizeof(cfg), &s_enc.handle) != ESP_AUDIO_ERR_OK || !s_enc.handle) {
        ESP_LOGE(TAG, "Opus エンコーダーを開けませんでした (rate %lu)", (unsigned long)rate);
        s_enc.handle = nullptr;
        return false;
    }
    s_enc.rate = rate;
    esp_opus_enc_get_frame_size(s_enc.handle, &s_enc.in_size, &s_enc.out_size);

    // OpusHead が取れれば、その pre-skip を使う
    esp_audio_enc_info_t info = {};
    if (esp_opus_enc_get_info(s_enc.handle, &info) == ESP_AUDIO_ERR_OK && info.codec_spec_info &&
        info.spec_info_len >= 12 && memcmp(info.codec_spec_info, "OpusHead", 8) == 0) {
        s_enc.pre_skip = info.codec_spec_info[10] | (info.codec_spec_info[11] << 8);
    }

    if (static_cast<size_t>(s_enc.out_size) > s_out_cap) {
        heap_caps_free(s_out);
        s_out = static_cast<uint8_t*>(heap_caps_malloc(s_enc.out_size, MALLOC_CAP_8BIT));
        s_out_cap = s_out ? s_enc.out_size : 0;
    }
    ESP_LOGI(TAG, "Opus %lu Hz, in %d B, out %d B, pre-skip %u", (unsigned long)rate, s_enc.in_size, s_enc.out_size,
             s_enc.pre_skip);
    return s_out != nullptr;
}

void page_sink(const uint8_t* h, size_t hl, const uint8_t* b, size_t bl) { storage::write(h, hl, b, bl); }

bool start_file(uint32_t rate) {
    if (s_enc.rate != rate || !s_enc.handle) {
        if (!open_encoder(rate)) return false;
    } else {
        esp_opus_enc_reset(s_enc.handle);
    }

    struct tm t;
    rtc_clock::local_now(&t);
    char name[24], path[48], date[40];
    strftime(name, sizeof(name), "%Y%m%d_%H%M%S.opus", &t);
    snprintf(path, sizeof(path), "%s/%s", storage::kRecDir, name);
    strftime(date, sizeof(date), "DATE=%Y-%m-%dT%H:%M:%S", &t);

    if (!storage::open_file(path)) return false;
    s_ogg.begin(esp_random(), rate, s_enc.pre_skip, kVendor, date, page_sink);
    s_file_open = true;
    s_file_rate = rate;
    s_file_frames = 0;
    state_set_file_name(name);
    g_state.rec_audio_ms.store(0);
    g_state.files_written.fetch_add(1);
    g_state.recording.store(true);
    event_log("録音開始 %s", name);
    return true;
}

void finish_file() {
    if (!s_file_open) return;
    s_ogg.flush(true);
    storage::close_file();
    s_file_open = false;
    g_state.recording.store(false);
    event_log("録音終了 %lu 秒", (unsigned long)(s_file_frames / 50));
    storage::flush_log();  // 「録音終了」まで SD に確定する（止めてすぐ SD を抜かれても残るように）
}

// UI が読むまでの最大値を残す（UI は exchange で読み、リセットする）
void store_max(std::atomic<int16_t>& a, int16_t v) {
    int16_t cur = a.load();
    while (v > cur && !a.compare_exchange_weak(cur, v)) {
    }
}
void store_max(std::atomic<uint32_t>& a, uint32_t v) {
    uint32_t cur = a.load();
    while (v > cur && !a.compare_exchange_weak(cur, v)) {
    }
}

// 1 フレームを Opus にして今のファイルに書く。設定した長さに達したらファイルを閉じる（次のフレームで新しいファイルを開く）
void encode_frame(int16_t* pcm, size_t n) {
    esp_audio_enc_in_frame_t in = {};
    in.buffer = reinterpret_cast<uint8_t*>(pcm);
    in.len = n * sizeof(int16_t);
    esp_audio_enc_out_frame_t out = {};
    out.buffer = s_out;
    out.len = s_out_cap;
    int64_t t0 = esp_timer_get_time();
    esp_audio_err_t err = esp_opus_enc_process(s_enc.handle, &in, &out);
    store_max(g_state.encode_us_max, static_cast<uint32_t>(esp_timer_get_time() - t0));
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "encode error %d", err);
        return;
    }

    s_ogg.add_packet(s_out, out.encoded_bytes, kSamples48kPerFrame);
    s_file_frames++;
    g_state.rec_audio_ms.store(s_file_frames * 20);
    uint32_t split = g_state.split_ms.load();
    if (split != 0 && s_file_frames * 20 >= split) finish_file();
}

// 途切れが始まってから今までの分の無音を、max_frames を上限に書く
void fill_gap(uint32_t max_frames) {
    static int16_t silence[kMaxFrameSamples];  // 0 のまま使う
    uint32_t due = static_cast<uint32_t>((esp_timer_get_time() - s_gap_start_us) / 20000);
    for (uint32_t i = 0; i < max_frames && s_gap_frames < due; i++) {
        // 途切れの間にファイルの区切りが来たら、続きのファイルを開いて無音を書き続ける
        if (!s_file_open && !start_file(s_file_rate)) return;
        encode_frame(silence, s_file_rate / 50);
        s_gap_frames++;
    }
}

void end_gap() {
    if (!s_gap) return;
    fill_gap(UINT32_MAX);
    s_gap = false;
    event_log("途切れていた %lu.%lu 秒を無音で埋めました", (unsigned long)(s_gap_frames / 50),
              (unsigned long)(s_gap_frames % 50 / 5));
}

void audio_task(void*) {
    static int16_t frame[kMaxFrameSamples];
    uint32_t gen = g_state.source_gen.load();
    uint32_t rate = g_state.sample_rate.load();

    for (;;) {
        // 音声接続が開いた・閉じた
        uint32_t g = g_state.source_gen.load();
        if (g != gen) {
            gen = g;
            rate = g_state.sample_rate.load();
            g_pcm_ring.discard_all();
            if (rate == 0) {
                if (s_file_open) {
                    s_gap = true;
                    s_gap_start_us = esp_timer_get_time();
                    s_gap_frames = 0;
                }
            } else {
                // つながり直した時刻までの無音を書き切ってから、続きを書く
                end_gap();
                // サンプルレートが変わったらファイルを分ける（同じなら同じファイルに続けて書く）
                if (s_file_open && rate != s_file_rate) finish_file();
            }
        }

        bool want_rec = g_state.rec_requested.load() && g_state.sd_mounted.load();
        if (!want_rec) {
            if (s_file_open) finish_file();
            s_gap = false;
        }

        if (rate == 0) {
            // 書くのが遅れても、一度に書く量を抑えて画面などを止めない
            if (s_gap) fill_gap(25);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        size_t n = rate / 50;
        if (g_pcm_ring.available() < n) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        g_pcm_ring.read(frame, n);

        // レベル計測（AGC などをかける前の値）
        FrameLevel lv = measure_frame(frame, n);
        store_max(g_state.level_rms_x10, static_cast<int16_t>(lv.rms_db * 10));
        store_max(g_state.level_peak_x10, static_cast<int16_t>(lv.peak_db * 10));
        if (lv.clipped) g_state.clip_frames.fetch_add(1);
        g_state.frames_processed.fetch_add(1);

        if (!want_rec) continue;
        if (!s_file_open && !start_file(rate)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        encode_frame(frame, n);
    }
}

}  // namespace

void start() {
    g_pcm_ring.init(16000 * 4, &g_state.ring_overflows);  // 16kHz で 4 秒分
    // Opus エンコーダーはスタックを多く使う
    xTaskCreatePinnedToCore(audio_task, "audio", 40 * 1024, nullptr, 10, nullptr, 1);
}

}  // namespace audio_pipeline
