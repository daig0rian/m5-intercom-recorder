#include "audio_pipeline.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "app_state.h"
#include "bt_ag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "level_meter.h"
#include "pcm_ring.h"
#include "rtc_clock.h"
#include "storage.h"
#include "wav_writer.h"

static const char* TAG = "audio";

namespace audio_pipeline {
namespace {

constexpr int kMaxFrameSamples = 16000 / 50;  // 20ms @ 16kHz
// 届いた PCM をそのまま WAV に書く。SD へは、これだけのフレーム（1 秒分）をまとめて渡す
constexpr uint32_t kFramesPerChunk = 50;
constexpr size_t kChunkCap = kMaxFrameSamples * sizeof(int16_t) * kFramesPerChunk;
// FAT32 は 1 ファイル 4GB まで。「分割なし」でも、この大きさで区切る（16kHz で約 34 時間）
constexpr uint32_t kMaxFileBytes = 4000000000u;

uint8_t* s_chunk = nullptr;  // SD に渡す前にためておく PCM
size_t s_chunk_len = 0;

bool s_file_open = false;
uint32_t s_file_rate = 0;
uint32_t s_file_frames = 0;
uint32_t s_file_bytes = 0;  // 頭を含むファイルの長さ

// 録音中に音声接続が閉じている間（途切れ）は、経った時間の分だけ無音を書き、
// 録音の時間を実際の時間に合わせる（ファイル名の日時から動画と合わせられるように）。
// 受信の見張りが働いている間（音声接続は開いているが、まともな音が届いていない）も同じように無音にする
bool s_gap = false;
bool s_gap_resume = false;  // 次に届いたフレームで途切れを終え、そこから届いた音を書く
bool s_gap_from_loop = false;  // 同じ波形のくり返しが続いたので無音にしている（くり返しが止まれば戻す）
uint32_t s_gap_frames = 0;

// ---- 録音の時間軸 ------------------------------------------------------------
// 録音を始めてから書いたフレーム（ファイルの区切りをまたいで数える）と本体の時刻を比べ、
// 途切れのあと、届いた音が途切れの前と同じ遅れで並ぶところまで無音を足す。
// 遅れ＝本体の時刻 − 届いた最新の音の位置（書いたフレーム＋リングにある分）。
// 届き方（まとめて届くなど）で揺れるので、音が届いている間の値をならしておく
constexpr int64_t kFrameUs = 20 * 1000;
// 途切れの間は、本体の時刻よりこれだけ手前までを埋めておき、残りはつながり直して届いた最初の音に合わせて埋める
// （先に埋めすぎると、戻せずに遅れが残るため）
constexpr int64_t kGapLeadUs = 100 * 1000;

struct Timeline {
    bool valid = false;     // 録音中（最初のフレームを書いた）
    int64_t anchor_us = 0;  // 位置 0 にあたる本体の時刻
    uint32_t frames = 0;    // 書いたフレーム（無音を含む）
    int64_t lag_us = 0;     // ふだんの遅れ（ならした値）
};
Timeline s_tl;

void timeline_reset() { s_tl = Timeline{}; }

// 届いた音のフレームを書く直前に呼ぶ。buffered_us は、このフレームを読む前にリングにあった分
void timeline_track(int64_t now, int64_t buffered_us) {
    if (!s_tl.valid) {
        s_tl.valid = true;
        s_tl.anchor_us = now - buffered_us;
        return;
    }
    int64_t lag = now - s_tl.anchor_us - static_cast<int64_t>(s_tl.frames) * kFrameUs - buffered_us;
    s_tl.lag_us += (lag - s_tl.lag_us) / 256;  // 5 秒ほどでならす
}

// ---- 受信の見張り ----------------------------------------------------------
// 音声接続が開いたまま、音がほとんど届かなくなる、または届く音が同じ波形のくり返しになる
// （Bluedroid の欠損補間が、壊れたフレームを直前の波形のくり返しで埋め続けている）ことがある。
// 2 秒ごとに確かめ、そうなっていたら音声接続を開き直す。
// 受信パケットの誤りが多い（電波が悪い）ときは、すぐには開き直さない（離れていく途中などで、
// 開き直すとかえって音が戻らなくなるため）。それでも 10 秒続いたら開き直す。
constexpr int64_t kWatchWindowUs = 2 * 1000 * 1000;
constexpr int64_t kWatchRadioWaitUs = 10 * 1000 * 1000;
constexpr int kRepeatMinLag = 56;   // Bluedroid の欠損補間がくり返す周期（16kHz のサンプル数）の範囲
constexpr int kRepeatMaxLag = 311;
// 同じ波形のくり返しがこれだけ続いたら、2 秒の見張りを待たずに無音にする（10 フレーム = 200ms）。
// 欠損補間は弱めずにくり返し続け、大きな雑音に変わることもあるため。これより短いくり返し（短い抜けの補間）はそのまま書く
constexpr uint32_t kMuteRepeatFrames = 10;
uint32_t s_repeat_run = 0;  // くり返しのフレームが続いている数

struct Watch {
    int64_t start_us = 0;
    uint32_t rx_bytes = 0;
    uint32_t frames = 0;   // 窓の中で読んだフレーム
    uint32_t repeats = 0;  // そのうち、前の波形のくり返しだったフレーム
    bool bad = false;      // まともな音が届いていない
    int64_t bad_since_us = 0;
    int64_t last_reopen_us = 0;
    int reopens = 0;       // 直らないまま開き直した回数
};
Watch s_watch;
int16_t s_prev_frame[kMaxFrameSamples];
bool s_prev_valid = false;

void watch_reset_window() {
    s_watch.start_us = esp_timer_get_time();
    s_watch.rx_bytes = g_state.rx_bytes.load();
    s_watch.frames = 0;
    s_watch.repeats = 0;
}

// 音声接続が開いた・閉じたとき。見張りの窓を始め直す（悪い状態かどうかと、開き直した回数は残す）
void watch_source_changed() {
    watch_reset_window();
    s_prev_valid = false;
    s_repeat_run = 0;
}

// このフレームが、前のフレームから続く同じ波形のくり返しか（ある周期で全サンプルが一致する）。
// 自然な音ではまず起きない。ほとんど無音のフレームは調べない（無音は、どの周期でも一致してしまう）
bool is_repeat(const int16_t* cur, size_t n) {
    if (!s_prev_valid || n != static_cast<size_t>(kMaxFrameSamples)) return false;
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += static_cast<uint32_t>(cur[i] < 0 ? -cur[i] : cur[i]);
    if (sum < n * 4) return false;
    for (int lag = kRepeatMinLag; lag <= kRepeatMaxLag; lag++) {
        size_t i = 0;
        for (; i < n; i++) {
            int16_t past = i >= static_cast<size_t>(lag) ? cur[i - lag] : s_prev_frame[n + i - lag];
            if (cur[i] != past) break;
        }
        if (i == n) return true;
    }
    return false;
}

void start_gap();

// 2 秒の窓が終わるたびに確かめる。rate は今の音声接続のサンプルレート
void watch_check(uint32_t rate) {
    int64_t now = esp_timer_get_time();
    int64_t el = now - s_watch.start_us;
    if (el < kWatchWindowUs) return;
    if (!bt_ag::audio_wanted()) {  // 閉じている最中（閉じた知らせを待っている間）は、届かなくて当然
        watch_reset_window();
        return;
    }
    uint32_t got = g_state.rx_bytes.load() - s_watch.rx_bytes;
    uint32_t expected = static_cast<uint32_t>(static_cast<int64_t>(rate) * 2 * el / 1000000);
    bool starving = got * 2 < expected;
    bool looping = s_watch.frames >= 25 && s_watch.repeats * 4 >= s_watch.frames * 3;
    uint32_t got_pct = expected ? got * 100 / expected : 0;
    watch_reset_window();

    if (!starving && !looping) {
        if (s_watch.bad) {
            s_watch.bad = false;
            s_watch.reopens = 0;
            if (s_gap) s_gap_resume = true;  // 開き直さずに戻った。次に届いたフレームから書く
            event_log("音声がまた届くようになりました");
        }
        return;
    }

    int drop = g_state.pkt_drop_x10.load();  // -1 = まだ分からない
    if (!s_watch.bad) {
        s_watch.bad = true;
        s_watch.bad_since_us = now;
        char err[24] = "不明";
        if (drop >= 0) snprintf(err, sizeof(err), "%d.%d%%", drop / 10, drop % 10);
        if (starving) event_log("音声がほとんど届きません（2 秒で %lu%%、受信の誤り %s）", (unsigned long)got_pct, err);
        else event_log("届く音が同じ波形のくり返しです（受信の誤り %s）", err);
    }
    // 録音中なら、ここから無音にする（開き直したあとも直っていなければ、また無音にする）。
    // 届かなかった分も、時間軸に合わせて埋める
    if (s_gap) s_gap_resume = false;
    else if (s_tl.valid) start_gap();

    bool radio = drop >= 500;  // 受信パケットの半分以上に誤り
    if (radio && now - s_watch.bad_since_us < kWatchRadioWaitUs) return;
    // 開き直しても直らないときは、間を空けて試し続ける（10 秒、10 秒、それからは 60 秒ごと）
    int64_t wait_us = s_watch.reopens == 0 ? 0 : s_watch.reopens < 3 ? 10 * 1000000LL : 60 * 1000000LL;
    if (s_watch.reopens > 0 && now - s_watch.last_reopen_us < wait_us) return;
    if (bt_ag::audio_reopen()) {
        s_watch.reopens++;
        s_watch.last_reopen_us = now;
    }
}

void flush_chunk() {
    if (s_chunk_len) storage::write(s_chunk, s_chunk_len, nullptr, 0);
    s_chunk_len = 0;
}

bool start_file(uint32_t rate) {
    struct tm t;
    rtc_clock::local_now(&t);
    char name[24], path[48];
    strftime(name, sizeof(name), "%Y%m%d_%H%M%S.wav", &t);
    snprintf(path, sizeof(path), "%s/%s", storage::kRecDir, name);

    // 頭の長さは 0 で書いておき、書き込みを確定するたびに書き直してもらう
    uint8_t header[wav::kHeaderBytes];
    wav::make_header(header, rate, 0);
    if (!storage::open_file(path, true)) return false;
    if (!storage::write(header, sizeof(header), nullptr, 0)) {
        storage::close_file();
        return false;
    }
    s_file_open = true;
    s_file_rate = rate;
    s_file_frames = 0;
    s_file_bytes = sizeof(header);
    s_chunk_len = 0;
    state_set_file_name(name);
    g_state.rec_audio_ms.store(0);
    g_state.files_written.fetch_add(1);
    g_state.recording.store(true);
    ESP_LOGI(TAG, "WAV %lu Hz", (unsigned long)rate);
    event_log("録音開始 %s", name);
    return true;
}

void finish_file() {
    if (!s_file_open) return;
    flush_chunk();
    storage::close_file();
    s_file_open = false;
    g_state.recording.store(false);
    event_log("録音終了 %lu 秒", (unsigned long)(s_file_frames / 50));
    storage::flush_log();  // 「録音終了」まで SD に確定する（止めてすぐ SD を抜かれても残るように）
    ESP_LOGI(TAG, "audio タスクのスタックの残り（最小）%u バイト", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
}

// UI が読むまでの最大値を残す（UI は exchange で読み、リセットする）
void store_max(std::atomic<int16_t>& a, int16_t v) {
    int16_t cur = a.load();
    while (v > cur && !a.compare_exchange_weak(cur, v)) {
    }
}

// 1 フレームを今のファイルに書く（1 秒分たまったら SD に渡す）。設定した長さか、1 ファイルの上限に達したら
// ファイルを閉じる（次のフレームで新しいファイルを開く）
void write_frame(const int16_t* pcm, size_t n) {
    size_t bytes = n * sizeof(int16_t);
    memcpy(s_chunk + s_chunk_len, pcm, bytes);
    s_chunk_len += bytes;
    if (s_chunk_len + bytes > kChunkCap || s_chunk_len >= bytes * kFramesPerChunk) flush_chunk();
    s_file_frames++;
    s_file_bytes += bytes;
    s_tl.frames++;
    g_state.rec_audio_ms.store(s_file_frames * 20);
    uint32_t split = g_state.split_ms.load();
    if ((split != 0 && s_file_frames * 20 >= split) || s_file_bytes > kMaxFileBytes - bytes) finish_file();
}

// 書いたフレームが target になるまで、無音を max_frames を上限に書く
void fill_to(int64_t target, uint32_t max_frames) {
    static int16_t silence[kMaxFrameSamples];  // 0 のまま使う
    for (uint32_t i = 0; i < max_frames && static_cast<int64_t>(s_tl.frames) < target; i++) {
        // 途切れの間にファイルの区切りが来たら、続きのファイルを開いて無音を書き続ける
        if (!s_file_open && !start_file(s_file_rate)) return;
        write_frame(silence, s_file_rate / 50);
        s_gap_frames++;
    }
}

// 途切れの間、本体の時刻の少し手前までの無音を書く
void fill_gap(uint32_t max_frames) {
    if (!s_tl.valid) return;
    int64_t pos = esp_timer_get_time() - s_tl.anchor_us - s_tl.lag_us - kGapLeadUs;
    if (pos > 0) fill_to(pos / kFrameUs, max_frames);
}

void start_gap() {
    s_gap = true;
    s_gap_resume = false;
    s_gap_from_loop = false;
    s_gap_frames = 0;
}

// 途切れを終える。届いた音（リングにある分を含む）が、途切れの前と同じ遅れで並ぶところまで無音を書く。
// buffered_us は、いまリングにある分（ないときは 0）
void end_gap(int64_t now, int64_t buffered_us) {
    if (!s_gap) return;
    if (s_tl.valid) {
        int64_t pos = now - s_tl.anchor_us - s_tl.lag_us - buffered_us;
        fill_to((pos + kFrameUs / 2) / kFrameUs, UINT32_MAX);
    }
    s_gap = false;
    s_gap_resume = false;
    s_gap_from_loop = false;
    event_log("途切れていた %lu.%lu 秒を無音で埋めました", (unsigned long)(s_gap_frames / 50),
              (unsigned long)(s_gap_frames % 50 / 5));
}

void audio_task(void*) {
    static int16_t frame[kMaxFrameSamples];
    uint32_t gen = g_state.source_gen.load();
    uint32_t rate = g_state.sample_rate.load();
    watch_source_changed();

    for (;;) {
        // 音声接続が開いた・閉じた
        uint32_t g = g_state.source_gen.load();
        if (g != gen) {
            gen = g;
            rate = g_state.sample_rate.load();
            g_pcm_ring.discard_all();
            watch_source_changed();
            if (rate == 0) {
                // 受信の見張りで無音にしている途中なら、そのまま続ける
                if (s_tl.valid && !s_gap) start_gap();
                s_gap_resume = false;
            } else if (s_tl.valid && rate != s_file_rate) {
                // サンプルレートが変わったらファイルを分ける（途切れの分は、いまの時刻まで埋めてから閉じる）
                end_gap(esp_timer_get_time(), 0);
                finish_file();
                timeline_reset();
            } else if (s_gap) {
                // 同じファイルに続けて書く。最初に届いたフレームの位置に合わせて途切れを終える
                s_gap_resume = true;
            }
        }

        // SD が満杯の手前になったら、ファイルを閉じて止める（録音の頼みは UI が下ろして知らせる）
        bool want_rec = g_state.rec_requested.load() && g_state.sd_mounted.load() && !g_state.sd_full.load();
        if (!want_rec) {
            if (s_file_open) finish_file();
            s_gap = false;
            s_gap_resume = false;
            timeline_reset();
        }

        if (rate == 0) {
            // 書くのが遅れても、一度に書く量を抑えて画面などを止めない
            if (s_gap) fill_gap(25);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        watch_check(rate);
        if (s_gap) fill_gap(25);  // 受信の見張りで無音にしている間
        size_t n = rate / 50;
        size_t avail = g_pcm_ring.available();
        if (avail < n) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        int64_t now = esp_timer_get_time();
        int64_t buffered_us = static_cast<int64_t>(avail) * 1000000 / rate;
        g_pcm_ring.read(frame, n);
        bool rep = is_repeat(frame, n);
        s_watch.frames++;
        if (rep) s_watch.repeats++;
        s_repeat_run = rep ? s_repeat_run + 1 : 0;
        memcpy(s_prev_frame, frame, n * sizeof(int16_t));
        s_prev_valid = n == static_cast<size_t>(kMaxFrameSamples);

        // レベル計測（AGC などをかける前の値）
        FrameLevel lv = measure_frame(frame, n);
        store_max(g_state.level_rms_x10, static_cast<int16_t>(lv.rms_db * 10));
        store_max(g_state.level_peak_x10, static_cast<int16_t>(lv.peak_db * 10));
        if (lv.clipped) g_state.clip_frames.fetch_add(1);
        g_state.frames_processed.fetch_add(1);

        // 同じ波形のくり返しが続いたら、このフレームから無音にする。くり返しが止まったら、そこから書く
        // （止まらずに 2 秒の見張りが気づいたら、開き直しのほうに任せる）
        if (s_repeat_run == kMuteRepeatFrames && s_tl.valid && !s_gap) {
            start_gap();
            s_gap_from_loop = true;
        } else if (!rep && s_gap_from_loop && !s_watch.bad) {
            s_gap_resume = true;
        }

        if (!want_rec || (s_gap && !s_gap_resume)) continue;  // 無音にしている間は、届いた音を書かない
        if (s_gap) end_gap(now, buffered_us);
        if (!s_file_open && !start_file(rate)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        timeline_track(now, buffered_us);
        write_frame(frame, n);
    }
}

}  // namespace

void start() {
    g_pcm_ring.init(16000 * 4, &g_state.ring_overflows);  // 16kHz で 4 秒分
    s_chunk = static_cast<uint8_t*>(heap_caps_malloc(kChunkCap, MALLOC_CAP_SPIRAM));
    if (!s_chunk) s_chunk = static_cast<uint8_t*>(heap_caps_malloc(kChunkCap, MALLOC_CAP_8BIT));
    xTaskCreatePinnedToCore(audio_task, "audio", 12 * 1024, nullptr, 10, nullptr, 1);
}

}  // namespace audio_pipeline
