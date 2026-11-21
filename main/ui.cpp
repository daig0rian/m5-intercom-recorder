#include "ui.h"

#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "M5Unified.h"
#include "bt_ag.h"
#include "esp_system.h"
#include "rtc_clock.h"
#include "settings.h"
#include "sound.h"
#include "spi_lock.h"
#include "storage.h"

namespace ui {
namespace {

// ---- 見た目 ----------------------------------------------------------------

constexpr int kW = 320;
constexpr int kH = 240;
constexpr int kBarH = 22;         // メニューなどの見出し
constexpr int kMainHeaderH = 42;  // メイン画面のヘッダー（2 段）
constexpr int kBtnY = 214;  // 下のボタン表示
constexpr int kBtnH = kH - kBtnY;

constexpr uint16_t kBg = 0x0000;
constexpr uint16_t kPanel = 0x2104;
constexpr uint16_t kText = 0xFFFF;
constexpr uint16_t kDim = 0x8410;
constexpr uint16_t kAccent = 0x05FF;
constexpr uint16_t kGreen = 0x07E0;
constexpr uint16_t kYellow = 0xFFE0;
constexpr uint16_t kOrange = 0xFD20;
constexpr uint16_t kRed = 0xF800;

const lgfx::IFont* kFont12 = &fonts::efontJA_12;
const lgfx::IFont* kFont16 = &fonts::efontJA_16;

constexpr uint8_t kBrightness[] = {160, 80, 30};
constexpr const char* kBrightnessName[] = {"高", "中", "低"};

// 描画の間隔。メーターだけは別に、録音していないときは速く描き直す
constexpr uint32_t kFullIntervalMs = 1000;  // 全体の描き直しは 1 秒ごと（操作したときはすぐ）。多いと Bluetooth の送信が間に合わなくなる
constexpr uint32_t kVuIntervalIdleMs = 33;
constexpr uint32_t kVuIntervalRecMs = 100;

// LED 風のレベルメーター。3dB 刻みで 20 個。いちばん上は音割れの表示
constexpr int kVuX = 8;
constexpr int kVuY = kMainHeaderH + 4;
constexpr int kVuW = kW - 16;
constexpr int kVuH = 22;
constexpr int kLeds = 20;
constexpr float kLedStepDb = 3.0f;
constexpr float kVuFloorDb = -60.0f;
constexpr float kClipLedDb = -1.0f;
constexpr uint32_t kHoldMs = 1500;          // ピークホールドを保つ時間
constexpr float kHoldFallDbPerSec = 20.0f;  // その後ピークホールドが下がる速さ

enum class Screen { Main, Menu, Devices, Log, Sync };

M5Canvas s_cv(&M5.Display);
M5Canvas s_vu(&M5.Display);
M5Canvas s_logv(&M5.Display);  // ログ画面の文字の部分（2 色。内部 RAM に置いて、なぞる間はここだけ描き直す）
Screen s_screen = Screen::Main;
int s_brightness = 0;

char s_toast[64] = "";
uint32_t s_toast_until = 0;

float s_bar_db = kVuFloorDb;
float s_hold_db = kVuFloorDb;
uint32_t s_hold_ms = 0;
uint32_t s_meter_ms = 0;

// 1 秒ごとの統計
uint32_t s_stat_ms = 0;
uint32_t s_rate_last_bytes = 0;
uint32_t s_rx_kbps_x10 = 0;
uint32_t s_encode_us = 0;
int s_battery = -1;
bool s_charging = false;


void toast(const char* msg) {
    strlcpy(s_toast, msg, sizeof(s_toast));
    s_toast_until = millis32() + 2500;
}

// ---- 描画の部品 ----------------------------------------------------------

void text(lgfx::LovyanGFX& c, const lgfx::IFont* font, uint16_t color, int x, int y, const char* s,
          textdatum_t datum = textdatum_t::top_left) {
    c.setFont(font);
    c.setTextColor(color);
    c.setTextDatum(datum);
    c.drawString(s, x, y);
}

void text(const lgfx::IFont* font, uint16_t color, int x, int y, const char* s,
          textdatum_t datum = textdatum_t::top_left) {
    text(s_cv, font, color, x, y, s, datum);
}

// disabled_mask: ビット i が立っているボタンは灰色で描く（押せないことを示す）
void buttons(const char* a, const char* b, const char* c, unsigned disabled_mask = 0) {
    const char* labels[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        int x = i * 107;
        if (!labels[i] || !*labels[i]) continue;
        bool enabled = !(disabled_mask & (1u << i));
        s_cv.fillRoundRect(x + 3, kBtnY + 1, 101, kBtnH - 2, 5, kPanel);
        text(kFont16, enabled ? kText : kDim, x + 53, kBtnY + kBtnH / 2, labels[i], textdatum_t::middle_center);
    }
}

// LED の何番目まで点くか（-1 = どれも点かない）
int led_index(float db) {
    if (db >= kClipLedDb) return kLeds - 1;
    int i = static_cast<int>(floorf((db - kVuFloorDb) / kLedStepDb)) - 1;
    return std::min(i, kLeds - 2);
}

void led_colors(int i, uint16_t* on, uint16_t* off) {
    float threshold = kVuFloorDb + (i + 1) * kLedStepDb;
    if (i == kLeds - 1 || threshold >= -3.0f) {
        *on = kRed;
        *off = 0x4000;
    } else if (threshold >= -12.0f) {
        *on = kYellow;
        *off = 0x4200;
    } else {
        *on = kGreen;
        *off = 0x0180;
    }
}

void draw_leds(lgfx::LovyanGFX& c, int x, int y) {
    int top = led_index(s_bar_db);
    int hold = led_index(s_hold_db);
    for (int i = 0; i < kLeds; i++) {
        uint16_t on, off;
        led_colors(i, &on, &off);
        int x0 = x + i * kVuW / kLeds;
        int x1 = x + (i + 1) * kVuW / kLeds - 2;
        c.fillRect(x0, y, x1 - x0, kVuH, (i <= top || i == hold) ? on : off);
    }
}

void vu_scale(int y) {
    static const int marks[] = {0, 4, 8, 12, 16, 18, 20};  // LED の境目
    for (int m : marks) {
        int px = kVuX + m * kVuW / kLeds - (m == kLeds ? 2 : 0);
        char b[8];
        snprintf(b, sizeof(b), "%d", static_cast<int>(kVuFloorDb + m * kLedStepDb));
        text(kFont12, kDim, std::clamp(px, kVuX + 8, kVuX + kVuW - 8), y, b, textdatum_t::top_center);
    }
}

void fmt_hms(char* out, size_t len, uint32_t sec) {
    snprintf(out, len, "%02lu:%02lu:%02lu", (unsigned long)(sec / 3600), (unsigned long)(sec / 60 % 60),
             (unsigned long)(sec % 60));
}

// 接続の状態（メイン画面のヘッダー下段）
const char* link_label() {
    switch (g_state.bt_state.load()) {
        case BtState::Off: return "Bluetooth 停止";
        case BtState::Idle: return "未接続";
        case BtState::Discovering: return "検索中";
        case BtState::Connecting: return "接続中…";
        case BtState::Connected: return "接続済み（音声なし）";
        case BtState::AudioOn:
            return g_state.codec.load() == Codec::Msbc ? "音声 mSBC 16kHz" : "音声 CVSD 8kHz";
    }
    return "";
}

// SD カードの形（右上の角を落とした四角）。入っていなければ中抜きに斜線
void sd_icon(int x, int y, bool mounted) {
    constexpr int w = 14, h = 18, cut = 5;
    if (mounted) {
        s_cv.fillRect(x, y, w, h, kText);
        s_cv.fillTriangle(x + w - cut, y, x + w, y, x + w, y + cut, kPanel);
        for (int i = 0; i < 3; i++) s_cv.fillRect(x + 3 + i * 3, y + 2, 2, 4, kPanel);  // 端子
    } else {
        s_cv.drawLine(x, y, x + w - cut, y, kDim);
        s_cv.drawLine(x + w - cut, y, x + w, y + cut, kDim);
        s_cv.drawLine(x + w, y + cut, x + w, y + h, kDim);
        s_cv.drawLine(x + w, y + h, x, y + h, kDim);
        s_cv.drawLine(x, y + h, x, y, kDim);
        for (int d = 0; d < 2; d++) s_cv.drawLine(x - 2 + d, y + h + 1, x + w + 1 + d, y - 2, kRed);
    }
}

// 稲妻（幅 7、高さ 12）
void bolt(int x, int y, uint16_t color) {
    s_cv.fillTriangle(x + 5, y, x, y + 7, x + 4, y + 7, color);
    s_cv.fillTriangle(x + 3, y + 5, x + 7, y + 5, x + 2, y + 12, color);
}

// 電池の形に残量を塗り、中に % を書く。充電中は枠を黄色にし、＋端子寄りに稲妻を置く
void battery_icon(int x, int y) {
    constexpr int w = 36, h = 16;
    uint16_t frame = s_charging ? kYellow : kText;
    s_cv.drawRect(x, y, w, h, frame);
    s_cv.fillRect(x + w, y + 5, 3, h - 10, frame);
    if (s_battery >= 0) {
        uint16_t fill = s_battery < 20 ? 0x8000 : s_battery < 50 ? 0x6300 : 0x03E0;
        int fw = (w - 4) * std::clamp(s_battery, 0, 100) / 100;
        s_cv.fillRect(x + 2, y + 2, fw, h - 4, fill);
    }
    int text_w = s_charging ? w - 10 : w;  // 稲妻の分を空ける
    if (s_charging) bolt(x + w - 10, y + 2, kYellow);
    if (s_battery >= 0) {
        char b[12];
        snprintf(b, sizeof(b), "%d", s_battery);
        text(kFont12, kText, x + text_w / 2 + 1, y + h / 2 + 1, b, textdatum_t::middle_center);
    }
}

// ヘッダー右側の SD・電池・時刻
void header_icons() {
    char b[16];
    struct tm t;
    rtc_clock::local_now(&t);
    snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
    text(kFont16, kText, kW - 4, 3, b, textdatum_t::top_right);
    battery_icon(kW - 4 - 40 - 6 - 39, 3);
    sd_icon(kW - 4 - 40 - 6 - 39 - 10 - 14, 2, g_state.sd_mounted.load());
}

// メニューなどの画面の見出し
void status_bar(const char* title) {
    s_cv.fillRect(0, 0, kW, kBarH, kPanel);
    text(kFont16, kText, 6, 3, title);
    header_icons();
}

void draw_toast() {
    if (!s_toast[0] || millis32() > s_toast_until) return;
    s_cv.fillRoundRect(20, 90, kW - 40, 40, 6, kAccent);
    text(kFont16, kBg, kW / 2, 110, s_toast, textdatum_t::middle_center);
}

// ---- メーターと統計 ------------------------------------------------------

void update_meters(uint32_t now) {
    float dt = (now - s_meter_ms) / 1000.0f;
    s_meter_ms = now;
    int16_t rms = g_state.level_rms_x10.exchange(kNoLevel);
    int16_t peak = g_state.level_peak_x10.exchange(kNoLevel);
    if (g_state.sample_rate.load() == 0) {
        s_bar_db = s_hold_db = kVuFloorDb;
        return;
    }
    // 棒は前回からのフレームの値をそのまま出す（減衰させない。下がり方はピークホールドで見る）
    if (rms != kNoLevel) s_bar_db = std::max(rms / 10.0f, kVuFloorDb);

    if (now - s_hold_ms > kHoldMs) s_hold_db = std::max(s_hold_db - kHoldFallDbPerSec * dt, kVuFloorDb);
    if (peak != kNoLevel && peak / 10.0f >= s_hold_db) {
        s_hold_db = peak / 10.0f;
        s_hold_ms = now;
    }
}

// ---- 電池 --------------------------------------------------------------------
// 電池が 0% のまま続いたら、録音ファイルを守るため、電源長押しと同じ流れで電源を切る。
// 一瞬の電圧の落ち込みで誤って切らないよう、続けて kEmptyReadings 回 0% のときだけ。
// 外から給電されているとき（充電中・USB から給電中）と、電池がつながっていないときは切らない。
//
// 残量の % は、AXP2101（Core2 v1.1）は電源 IC の残量計の値を使う。AXP192（Core2 初期型・v1.3）は残量計を持たず、
// M5Unified は電池の電圧から直線で出す（3.3V = 0%、4.1V = 100%）。リポ電池の電圧は終わりに急に下がるので、
// それでは表示が最後に急に減り、0% になる前に電源 IC が電源を切ることもある。そこで AXP192 では、
// 録音中（消灯）に電池が切れるまで測った電圧の下がり方から、% が録音できる残り時間に比例するよう表で換算する。
// 0% は 3.5V とし、電源 IC が自分で切る 3.0V までに数分の余裕を残す（3.3V で切るより、録音できる時間が約 2% 減る）。
// 電圧は流れる電流で下がる（画面を点けると 10mV ほど）ので、電圧をならしてから換算し、電池で動いている間は
// 表示を増やさない。
struct LevelPoint {
    int16_t mv;
    int8_t pct;
};
// 電圧の高い順。録音中（消灯、約 135mA）に測った電圧。点の間は直線で結ぶ。
// 下の端は電圧が急に下がるので点を細かくする。4.1V より上は 100%（そこから先は外挿で、まだ測っていない）。
constexpr LevelPoint kAxp192Levels[] = {
    {4100, 100}, {4011, 90}, {3924, 80}, {3864, 70}, {3805, 60}, {3754, 50}, {3718, 40}, {3697, 30},
    {3673, 20},  {3621, 10}, {3600, 5},  {3583, 3},  {3566, 2},  {3545, 1},  {3500, 0},
};
constexpr uint32_t kSmoothMs = 60 * 1000;   // 電圧をならす時定数
constexpr uint32_t kSettleMs = 120 * 1000;  // 電池で動き始めてから、表示を増やさないようにするまで（ならしが落ち着くまで）

// 端数は切り上げる（0% は 0% の電圧ちょうどで、電源を切る合図になる。それまでは 1%）
int axp192_level(int mv) {
    constexpr int n = sizeof(kAxp192Levels) / sizeof(kAxp192Levels[0]);
    if (mv >= kAxp192Levels[0].mv) return 100;
    for (int i = 1; i < n; ++i) {
        const LevelPoint& hi = kAxp192Levels[i - 1];
        const LevelPoint& lo = kAxp192Levels[i];
        if (mv >= lo.mv) {
            int span = hi.mv - lo.mv;
            return lo.pct + ((hi.pct - lo.pct) * (mv - lo.mv) + span - 1) / span;
        }
    }
    return 0;
}

struct BatteryGauge {
    bool started = false;
    bool on_battery = false;
    float mv = 0;           // ならした電圧
    uint32_t since_ms = 0;  // 電池で動き始めた（または外からの給電になった）時刻
    uint32_t last_ms = 0;
    int level = 0;
};
BatteryGauge s_gauge;

// AXP192 の残量（%）。電池で動いているかどうかが変わったら、ならし直す（充電中は電圧が高く出るため）
int axp192_gauge(bool on_battery, int mv, uint32_t now) {
    BatteryGauge& g = s_gauge;
    if (!g.started || on_battery != g.on_battery) {
        g = BatteryGauge{true, on_battery, static_cast<float>(mv), now, now, 0};
    } else {
        g.mv += (mv - g.mv) * std::min(1.0f, static_cast<float>(now - g.last_ms) / kSmoothMs);
        g.last_ms = now;
    }
    int level = axp192_level(static_cast<int>(g.mv + 0.5f));
    if (on_battery && now - g.since_ms >= kSettleMs) level = std::min(level, g.level);
    g.level = level;
    return level;
}

constexpr int kEmptyReadings = 3;
constexpr int kVbusPresentMv = 4000;
constexpr int kBatteryPresentMv = 2500;
int s_empty_count = 0;
bool s_battery_empty = false;
int s_usb_logged = -1;  // ログに残した USB 給電の有無（-1 = まだ）

// USB から給電されているか。Core2 の初期型（AXP192）は USB が ACIN につながり、VBUS は 5V の端子（M-Bus・Grove）の側。
// 電池で動いているときも Core2 が 5V の端子へ昇圧して出すので、VBUS の電圧では USB の有無が分からない。
bool usb_power() {
    if (M5.Power.getType() == m5::Power_Class::pmic_axp192) return M5.Power.Axp192.isACIN();
    return M5.Power.getVBUSVoltage() > kVbusPresentMv;
}

// 電池の試験（電池が切れるまでの時間を測る）のときだけ true にする。電池で動いている間、
// 残量の % が変わったときと、変わらなくても kBatteryLogIntervalMs ごとに、前の行からの平均の電圧と放電電流をログに書く。
constexpr bool kBatteryTestLog = true;
constexpr uint32_t kBatteryLogIntervalMs = 5 * 60 * 1000;

struct BatteryTestLog {
    int level = -1;  // 最後に書いた残量（-1 = 電池で動き始めてから、まだ書いていない）
    uint32_t last_ms = 0;
    int32_t sum_mv = 0;
    int32_t sum_ma = 0;
    int32_t n = 0;
};
BatteryTestLog s_btlog;

void battery_test_log(bool on_battery, int level, int mv, uint32_t now) {
    if (!kBatteryTestLog) return;
    BatteryTestLog& b = s_btlog;
    if (!on_battery) {
        b = BatteryTestLog{};
        return;
    }
    b.sum_mv += mv;
    b.sum_ma += -M5.Power.getBatteryCurrent();  // 放電を正にする
    b.n++;
    if (b.level >= 0 && level == b.level && now - b.last_ms < kBatteryLogIntervalMs) return;
    event_log("電池 %d%%（平均 %ldmV・放電 %ldmA）", level, (long)(b.sum_mv / b.n), (long)(b.sum_ma / b.n));
    b.level = level;
    b.last_ms = now;
    b.sum_mv = b.sum_ma = b.n = 0;
}

void read_power() {
    uint32_t now = millis32();
    s_charging = M5.Power.isCharging() == m5::Power_Class::is_charging;
    bool usb = usb_power();
    int bat_mv = M5.Power.getBatteryVoltage();
    bool external = s_charging || usb;
    bool has_battery = bat_mv > kBatteryPresentMv;
    bool on_battery = !external && has_battery;
    if (M5.Power.getType() == m5::Power_Class::pmic_axp192) {
        s_battery = axp192_gauge(on_battery, bat_mv, now);
    } else {
        s_battery = M5.Power.getBatteryLevel();
    }
    if (s_usb_logged != static_cast<int>(usb)) {
        event_log(usb ? "USB から給電中 (電池 %dmV)" : "電池で動作中 (電池 %dmV)", bat_mv);
        s_usb_logged = usb;
    }
    battery_test_log(on_battery, s_battery, bat_mv, now);
    if (on_battery && s_battery == 0) {
        if (++s_empty_count == 1) event_log("電池残量 0%%");
    } else {
        s_empty_count = 0;
    }
    s_battery_empty = s_empty_count >= kEmptyReadings;
}

void update_stats(uint32_t now) {
    if (now - s_stat_ms < 1000) return;
    uint32_t bytes = g_state.rx_bytes.load();
    s_rx_kbps_x10 = (bytes - s_rate_last_bytes) * 10 / 1024 * 1000 / std::max<uint32_t>(now - s_stat_ms, 1);
    s_rate_last_bytes = bytes;
    s_stat_ms = now;
    s_encode_us = g_state.encode_us_max.exchange(0);
    read_power();
}

// ---- 各画面 --------------------------------------------------------------

void draw_main() {
    char b[96];
    uint32_t now = millis32();
    bool want = g_state.rec_requested.load();
    bool rec = g_state.recording.load();
    bool has_audio = g_state.sample_rate.load() != 0;

    // ヘッダー（2 段）。上: 録音の状態と SD・電池・時刻、下: 接続の状態と録音ファイル
    s_cv.fillRect(0, 0, kW, kMainHeaderH, kPanel);
    if (want && rec && has_audio) {
        s_cv.fillCircle(12, 11, 6, kRed);
        char hms[16];
        fmt_hms(hms, sizeof(hms), (now - g_state.rec_started_ms.load()) / 1000);
        snprintf(b, sizeof(b), "REC %s", hms);
        text(kFont16, kRed, 24, 3, b);
    } else if (want) {
        s_cv.fillCircle(12, 11, 6, kOrange);
        text(kFont16, kOrange, 24, 3, has_audio ? "録音準備中" : "音声待ち");
    } else if (g_state.loopback.load()) {
        text(kFont16, kAccent, 6, 3, "ループバック中");
    } else if (!g_state.sd_mounted.load()) {
        text(kFont16, kOrange, 6, 3, "SDカードがありません");
    } else {
        text(kFont16, kDim, 6, 3, "待機中");
    }
    header_icons();

    text(kFont12, kText, 6, 25, link_label());
    char fname[48];
    state_get_file_name(fname, sizeof(fname));
    if (fname[0]) {
        char t[16];
        fmt_hms(t, sizeof(t), g_state.rec_audio_ms.load() / 1000);
        snprintf(b, sizeof(b), "%s  %s", fname, t);
        text(kFont12, kDim, kW - 4, 25, b, textdatum_t::top_right);
    }

    // レベルメーター
    draw_leds(s_cv, kVuX, kVuY);
    vu_scale(kVuY + kVuH + 3);

    // 診断の数値
    int y = kVuY + kVuH + 20;
    const int lh = 14;
    // 欠けは直近 2 秒の受信パケットの割合（音声接続がないときは「-」）
    int drop = g_state.pkt_drop_x10.load();
    char drop_s[12];
    if (drop >= 0) snprintf(drop_s, sizeof(drop_s), "%d.%d%%", drop / 10, drop % 10);
    else strlcpy(drop_s, "-", sizeof(drop_s));
    snprintf(b, sizeof(b), "受信 %lu.%lukB/s  欠け %s  溢れ %lu  音割れ %lu", (unsigned long)(s_rx_kbps_x10 / 10),
             (unsigned long)(s_rx_kbps_x10 % 10), drop_s, (unsigned long)g_state.ring_overflows.load(),
             (unsigned long)g_state.clip_frames.load());
    text(kFont12, kText, 6, y, b);
    y += lh;
    uint32_t free_mb = g_state.sd_free_mb.load();
    snprintf(b, sizeof(b), "書込 %luKB  エラー %lu  エンコード %lu.%lums  空き %lu.%luGB",
             (unsigned long)(g_state.bytes_written.load() / 1024),
             (unsigned long)(g_state.write_errors.load() + g_state.write_queue_drops.load()),
             (unsigned long)(s_encode_us / 1000), (unsigned long)(s_encode_us % 1000 / 100),
             (unsigned long)(free_mb / 1024), (unsigned long)(free_mb % 1024 * 10 / 1024));
    text(kFont12, kText, 6, y, b);
    y += lh + 2;

    // 直近のイベント（ボタンの上まで）
    constexpr int kMaxLines = 8;
    char ev[kMaxLines][kEventLineLen];
    int fit = std::min((kBtnY - y) / lh, kMaxLines);
    int n = event_log_snapshot(ev, fit);
    for (int i = 0; i < n; i++, y += lh) text(kFont12, kAccent, 6, y, ev[i]);

    const char* a;
    BtState st = g_state.bt_state.load();
    char peer[40];
    state_get_peer_name(peer, sizeof(peer));
    if (st == BtState::Idle) a = peer[0] ? "BT接続" : "機器検索";
    else if (st == BtState::Discovering) a = "検索中";
    else a = "BT切断";
    buttons(a, "メニュー", want ? "停止" : "録音");
}

// メニューの行。画面に収まらない分は、指でなぞって上下にスクロールする
enum MenuRow {
    kRowPair,
    kRowAudio,
    kRowLoopback,
    kRowSync,
    kRowMicGain,
    kRowSplit,
    kRowLogView,
    kRowLogSd,
    kRowReconnect,
    kRowBrightness,
    kRowReset,
    kMenuRows
};
constexpr int kRowY0 = kBarH + 6;
constexpr int kRowPitch = 37;
constexpr int kRowH = 33;
constexpr int kMenuBottom = kBtnY - 3;  // 行を見せる範囲の下端
int s_menu_scroll = 0;

int menu_scroll_max() {
    return std::max(0, kRowY0 + (kMenuRows - 1) * kRowPitch + kRowH - kMenuBottom);
}

// 録音ファイルを分ける長さの選択肢
struct SplitOption {
    uint32_t sec;
    const char* name;
};
constexpr SplitOption kSplitOptions[] = {{0, "分割なし"}, {30, "30秒"},  {60, "1分"},   {300, "5分"},
                                         {900, "15分"},   {1800, "30分"}, {3600, "1時間"}};
constexpr int kSplitCount = sizeof(kSplitOptions) / sizeof(kSplitOptions[0]);

int split_index() {
    uint32_t sec = g_state.split_ms.load() / 1000;
    for (int i = 0; i < kSplitCount; i++) {
        if (kSplitOptions[i].sec == sec) return i;
    }
    return 5;  // 30 分
}
constexpr int kStepMinusX = 190;  // マイク感度の「−」
constexpr int kStepPlusX = 274;   // マイク感度の「＋」
constexpr int kStepW = 38;
constexpr int kRowNoteX = 120;   // 行の中の小さな添え書き（「非対応機器」など）の位置

// 「設定の初期化」: 押すと右のボタンが赤い「初期化する?」に変わり、それを押したら消す（押し間違いを防ぐ）
constexpr int kResetBtnW = 84;
constexpr int kResetBtnX = kW - 16 - kResetBtnW;
constexpr uint32_t kResetConfirmMs = 3000;  // 「初期化する?」を押すまでの猶予
uint32_t s_reset_armed_until = 0;

bool reset_armed() { return millis32() < s_reset_armed_until; }

// スライド式のトグル。busy は切り替え中（つまみを真ん中に置く）
void toggle_switch(int y_center, bool on, bool enabled, bool busy) {
    constexpr int w = 44, h = 22;
    int x = kW - 16 - w;
    int y = y_center - h / 2;
    uint16_t track = !enabled ? 0x2945 : busy ? 0x7A00 : on ? kAccent : 0x4208;
    uint16_t knob = enabled ? kText : 0x6B4D;
    s_cv.fillRoundRect(x, y, w, h, h / 2, track);
    int kx = busy ? x + w / 2 : on ? x + w - h / 2 : x + h / 2;
    s_cv.fillCircle(kx, y_center, h / 2 - 3, knob);
}

void step_button(int x, int y_center, const char* label, bool enabled) {
    s_cv.fillRoundRect(x, y_center - 12, kStepW, 25, 4, enabled ? 0x4208 : 0x2945);
    text(kFont16, enabled ? kText : kDim, x + kStepW / 2, y_center, label, textdatum_t::middle_center);
}

void draw_menu() {
    status_bar("メニュー");
    BtState st = g_state.bt_state.load();
    bool connected = st >= BtState::Connected;
    bool audio_on = st == BtState::AudioOn;

    s_menu_scroll = std::clamp(s_menu_scroll, 0, menu_scroll_max());
    s_cv.setClipRect(0, kBarH, kW, kMenuBottom - kBarH);  // 見出しとボタンに重ならないように
    for (int i = 0; i < kMenuRows; i++) {
        int y = kRowY0 + i * kRowPitch - s_menu_scroll;
        if (y + kRowH < kBarH || y > kMenuBottom) continue;  // 見えない行
        int yc = y + kRowH / 2;
        s_cv.fillRoundRect(6, y, kW - 12, kRowH, 5, kPanel);
        switch (i) {
            case kRowPair:
                text(kFont16, kText, 16, yc, "機器を探す（ペアリング）", textdatum_t::middle_left);
                break;
            case kRowAudio:
                text(kFont16, connected ? kText : kDim, 16, yc, "音声接続", textdatum_t::middle_left);
                toggle_switch(yc, audio_on, connected, bt_ag::audio_busy());
                break;
            case kRowLoopback:
                text(kFont16, audio_on ? kText : kDim, 16, yc, "ループバックモニター", textdatum_t::middle_left);
                toggle_switch(yc, g_state.loopback.load(), audio_on, false);
                break;
            case kRowSync:
                text(kFont16, kText, 16, yc, "同期信号", textdatum_t::middle_left);
                text(kFont12, kDim, kRowNoteX, yc, "映像と音を合わせる", textdatum_t::middle_left);
                break;
            case kRowMicGain: {
                // インカムが遠隔設定に対応していないときは灰色にして押せないようにする
                bool usable = connected && g_state.hf_remote_volume.load();
                text(kFont16, usable ? kText : kDim, 16, yc, "マイク感度", textdatum_t::middle_left);
                if (connected && !usable) text(kFont12, kDim, kRowNoteX, yc, "非対応機器", textdatum_t::middle_left);
                step_button(kStepMinusX, yc, "−", usable);
                step_button(kStepPlusX, yc, "＋", usable);
                int gain = g_state.hf_mic_gain.load();
                char b[8];
                snprintf(b, sizeof(b), gain >= 0 ? "%d" : "--", gain);
                text(kFont16, usable ? kText : kDim, (kStepMinusX + kStepW + kStepPlusX) / 2, yc, b,
                     textdatum_t::middle_center);
                break;
            }
            case kRowSplit: {
                // 録音中は変えられない（いまのファイルや次の区切りがどうなるか分かりにくくなるため）
                bool rec = g_state.rec_requested.load();
                uint16_t c = rec ? kDim : kText;
                text(kFont16, c, 16, yc, "ファイル分割", textdatum_t::middle_left);
                if (rec) text(kFont12, kDim, kRowNoteX, yc, "録音中変更不可", textdatum_t::middle_left);
                text(kFont16, c, kW - 16, yc, kSplitOptions[split_index()].name, textdatum_t::middle_right);
                break;
            }
            case kRowLogView:
                text(kFont16, kText, 16, yc, "ログ表示", textdatum_t::middle_left);
                break;
            case kRowLogSd:
                text(kFont16, kText, 16, yc, "ログを SD に保存", textdatum_t::middle_left);
                toggle_switch(yc, g_state.log_to_sd.load(), true, false);
                break;
            case kRowReconnect:
                text(kFont16, kText, 16, yc, "起動時に再接続", textdatum_t::middle_left);
                toggle_switch(yc, g_state.reconnect_on_boot.load(), true, false);
                break;
            case kRowBrightness:
                text(kFont16, kText, 16, yc, "画面の明るさ", textdatum_t::middle_left);
                text(kFont16, kText, kW - 16, yc, kBrightnessName[s_brightness], textdatum_t::middle_right);
                break;
            case kRowReset: {
                // 録音中は使えない（初期化は再起動するので、録音が止まる）
                bool rec = g_state.rec_requested.load();
                bool armed = !rec && reset_armed();
                text(kFont16, rec ? kDim : kText, 16, yc, "設定の初期化", textdatum_t::middle_left);
                if (rec) text(kFont12, kDim, kRowNoteX, yc, "録音中変更不可", textdatum_t::middle_left);
                s_cv.fillRoundRect(kResetBtnX, yc - 12, kResetBtnW, 25, 4, rec ? 0x2945 : armed ? kRed : 0x4208);
                text(kFont12, rec ? kDim : kText, kResetBtnX + kResetBtnW / 2, yc, armed ? "初期化する?" : "初期化",
                     textdatum_t::middle_center);
                break;
            }
        }
    }
    // スクロールできるときは、右端に今の位置を細い棒で示す
    int max = menu_scroll_max();
    if (max > 0) {
        int track_h = kMenuBottom - kRowY0;
        int bar_h = track_h * track_h / (track_h + max);
        int bar_y = kRowY0 + (track_h - bar_h) * s_menu_scroll / max;
        s_cv.fillRect(kW - 3, bar_y, 2, bar_h, kDim);
    }
    s_cv.clearClipRect();
    buttons("", "", "戻る");
}

// ---- ログ画面 --------------------------------------------------------------
// 全画面で 13 行。なぞると上下に動く。上へ戻して読んでいる間は、新しい行が来ても表示を動かさない

constexpr int kLogY0 = kBarH + 2;
constexpr int kLogLineH = 14;
constexpr int kLogLines = 13;
constexpr int kLogAreaH = kLogLines * kLogLineH;
constexpr uint32_t kLogFastIntervalMs = 33;  // なぞっている間の描き直しは 1 秒に最大 30 回

int s_log_offset = 0;           // いちばん下に見えている行が、最新から何行前か（0 = 最新に追従）
uint32_t s_log_seen_total = 0;  // 前に見たときまでに書かれた行数
bool s_log_new = false;         // 上へ戻している間に新しい行が来た
int s_log_drag_px = 0;          // なぞった量のうち、まだ行に換算していない分
bool s_log_fast_due = false;    // 文字の部分だけ描き直す

void sync_log_view() {
    uint32_t total = event_log_total();
    if (total != s_log_seen_total) {
        if (s_log_offset > 0) {
            s_log_offset += static_cast<int>(total - s_log_seen_total);  // 読んでいる位置を動かさない
            s_log_new = true;
        }
        s_log_seen_total = total;
    }
    s_log_offset = std::clamp(s_log_offset, 0, std::max(0, event_log_count() - kLogLines));
    if (s_log_offset == 0) s_log_new = false;
}

// 文字の部分を描く。s_cv（フルカラー）にも s_logv（2 色、色は番号）にも描けるよう色を渡す
void draw_log_lines(lgfx::LovyanGFX& c, int y0, uint16_t fg) {
    static char lines[kLogLines][kEventLineLen];
    int count = event_log_count();
    int end = count - s_log_offset;
    int start = std::max(0, end - kLogLines);
    int n = event_log_read(start, end - start, lines);
    for (int i = 0; i < n; i++) text(c, kFont12, fg, 6, y0 + i * kLogLineH, lines[i]);
    // 右端に、全体のどこを見ているかを細い棒で示す
    if (count > kLogLines) {
        int bar_h = std::max(8, kLogAreaH * kLogLines / count);
        int bar_y = y0 + (kLogAreaH - bar_h) * start / (count - kLogLines);
        c.fillRect(kW - 3, bar_y, 2, bar_h, fg);
    }
}

void draw_log() {
    sync_log_view();
    char title[48];
    snprintf(title, sizeof(title), "ログ  %d 行%s", event_log_count(), s_log_new ? "  ↓新着あり" : "");
    status_bar(title);
    draw_log_lines(s_cv, kLogY0, kAccent);
    buttons(s_log_offset > 0 ? "最新へ" : "", "", "戻る");
}

// なぞっている間は文字の部分だけを描き直す（画面全体は描き直さない）
void render_log_fast() {
    s_logv.fillScreen(0);
    draw_log_lines(s_logv, 0, 1);
    SpiLockGuard lock;
    s_logv.pushSprite(0, kLogY0);
}

// ---- 機器の画面 ------------------------------------------------------------
// Core2 が覚えている機器（ペアリングの鍵がある機器）の一覧。
// 上に、検索で見つかったまだペアリングしていない機器（行に「ペアリング」）、
// 下に、ペアリング済みの機器（最後に接続した機器に印。行に「削除」）を並べる。

struct DeviceRow {
    bool paired;
    uint8_t bda[6];
    char name[40];
    bool is_audio;
    int8_t rssi;
    bool last;
    bool connected;
};
constexpr int kMaxDeviceRows = 24;
constexpr int kDevRowY0 = kBarH + 4;
constexpr int kDevRowPitch = 31;
constexpr int kDevRowH = 28;
constexpr int kDevBottom = kBtnY - 3;
constexpr int kDevBtnW = 80;  // 行の右の「ペアリング」「削除」
constexpr int kDevBtnX = kW - 8 - kDevBtnW;
constexpr uint32_t kDeleteConfirmMs = 3000;  // 「削除」をもう一度押すまでの猶予

DeviceRow s_rows[kMaxDeviceRows];
int s_row_count = 0;
int s_dev_scroll = 0;
Screen s_devices_back = Screen::Menu;  // 「戻る」で戻る画面
uint8_t s_delete_armed[6] = {};
uint32_t s_delete_armed_until = 0;

bool delete_armed(const uint8_t* bda) {
    return millis32() < s_delete_armed_until && memcmp(s_delete_armed, bda, 6) == 0;
}

void build_device_rows() {
    s_row_count = 0;
    bt_ag::FoundDevice found[16];
    int nf = bt_ag::found_devices(found, 16);
    for (int i = 0; i < nf && s_row_count < kMaxDeviceRows; i++) {
        if (bt_ag::is_paired(found[i].bda)) continue;  // ペアリング済みは下にまとめて出す
        DeviceRow& r = s_rows[s_row_count++];
        r = DeviceRow{};
        memcpy(r.bda, found[i].bda, 6);
        if (found[i].name[0]) {
            strlcpy(r.name, found[i].name, sizeof(r.name));
        } else {
            snprintf(r.name, sizeof(r.name), "%02X:%02X:%02X:%02X:%02X:%02X", r.bda[0], r.bda[1], r.bda[2],
                     r.bda[3], r.bda[4], r.bda[5]);
        }
        r.is_audio = found[i].is_audio;
        r.rssi = found[i].rssi;
    }
    bt_ag::PairedDevice paired[8];
    int np = bt_ag::paired_devices(paired, 8);
    for (int i = 0; i < np && s_row_count < kMaxDeviceRows; i++) {
        DeviceRow& r = s_rows[s_row_count++];
        r = DeviceRow{};
        r.paired = true;
        memcpy(r.bda, paired[i].bda, 6);
        strlcpy(r.name, paired[i].name, sizeof(r.name));
        r.last = paired[i].last;
        r.connected = paired[i].connected;
    }
}

int dev_scroll_max() {
    return std::max(0, kDevRowY0 + (s_row_count - 1) * kDevRowPitch + kDevRowH - kDevBottom);
}

void draw_devices() {
    BtState st = g_state.bt_state.load();
    build_device_rows();
    status_bar(st == BtState::Discovering ? "機器  検索中…" : "機器");

    if (s_row_count == 0) {
        text(kFont16, kText, 10, 40, "ペアリングした機器はありません。");
        text(kFont16, kDim, 10, 64, "インカムをペアリング待機にしてください。");
        text(kFont16, kDim, 10, 88, "B+COM なら B2 ボタンを長押しします。");
    }
    s_dev_scroll = std::clamp(s_dev_scroll, 0, dev_scroll_max());
    for (int i = 0; i < s_row_count; i++) {
        const DeviceRow& r = s_rows[i];
        int y = kDevRowY0 + i * kDevRowPitch - s_dev_scroll;
        if (y + kDevRowH < kBarH || y > kDevBottom) continue;
        int yc = y + kDevRowH / 2;
        s_cv.setClipRect(0, kBarH, kW, kDevBottom - kBarH);
        s_cv.fillRoundRect(4, y, kW - 8, kDevRowH, 4, kPanel);

        // 右のボタン
        bool armed = r.paired && delete_armed(r.bda);
        uint16_t btn_bg = !r.paired ? kAccent : armed ? kRed : 0x4208;
        s_cv.fillRoundRect(kDevBtnX, y + 3, kDevBtnW - 4, kDevRowH - 6, 4, btn_bg);
        text(kFont12, !r.paired ? kBg : kText, kDevBtnX + (kDevBtnW - 4) / 2, yc,
             !r.paired ? "ペアリング" : armed ? "削除する?" : "削除", textdatum_t::middle_center);

        // 印（最後に接続 / 電波の強さ）
        int tag_right = kDevBtnX - 6;
        int name_right = tag_right;
        if (r.paired && r.last) {
            text(kFont12, kAccent, tag_right, yc, r.connected ? "接続中" : "最後に接続", textdatum_t::middle_right);
            name_right = tag_right - 64;
        } else if (!r.paired && r.rssi) {
            char b[8];
            snprintf(b, sizeof(b), "%d", r.rssi);
            text(kFont12, kDim, tag_right, yc, b, textdatum_t::middle_right);
            name_right = tag_right - 28;
        }

        // 名前（長ければ印の手前で切る）
        int clip_top = std::max(y, kBarH);
        int clip_bottom = std::min(y + kDevRowH, kDevBottom);
        s_cv.setClipRect(12, clip_top, std::max(0, name_right - 12), std::max(0, clip_bottom - clip_top));
        char b[48];
        snprintf(b, sizeof(b), "%s%s", (!r.paired && r.is_audio) ? "♪ " : "", r.name);
        text(kFont16, (r.paired || r.is_audio) ? kText : kDim, 12, yc, b, textdatum_t::middle_left);
    }
    s_cv.clearClipRect();
    int max = dev_scroll_max();
    if (max > 0) {
        int track_h = kDevBottom - kDevRowY0;
        int bar_h = track_h * track_h / (track_h + max);
        int bar_y = kDevRowY0 + (track_h - bar_h) * s_dev_scroll / max;
        s_cv.fillRect(kW - 3, bar_y, 2, bar_h, kDim);
    }
    // 検索できるのは未接続のときだけ（つながっている間は無線の時間を検索に取られ、音が途切れるため）
    buttons(st == BtState::Discovering ? "" : "検索", "", "戻る", st == BtState::Idle ? 0 : 1);
}

// ---- 同期信号 --------------------------------------------------------------
// 映像と録音を合わせるための画面。15 秒ごと（毎分 0・15・30・45 秒）に、時報と同じ並びで鳴らす。
// 12・13・14 秒に予報音を鳴らし、鳴っている間は真ん中の四角を白く光らせる。
// 15 秒ちょうどに正報音を鳴らし、鳴っている間は画面全体の白黒を反転する（液晶の反転の命令なので一瞬で切り替わる）。
// スピーカーの音をインカムのマイクに拾わせ、画面をカメラに写す。正報音の鳴り始めと反転の始まりで、映像と音を合わせる。
// 鳴らす時刻と光らせる時刻は sync_task が決め、画面の時刻の数字・四角・反転は SPI のロックを持って直接描き換える。

constexpr int kSyncPeriodSec = 15;
constexpr int kSyncFirstPipSec = 12;  // 15 秒周期の中で、予報音を鳴らし始める秒
constexpr int kSyncClockY = 47;       // 時刻の数字（Font7、高さ 48）
constexpr int kSyncClockH = 48;
constexpr int kSyncFlashX = 40;  // 光る四角
constexpr int kSyncFlashY = 103;
constexpr int kSyncFlashW = kW - 80;
constexpr int kSyncFlashH = 88;
constexpr uint8_t kSyncBrightness = 255;  // カメラに写りやすいよう、この画面の間は一番明るくする
// 鳴らす指示から光らせるまでの時間（ms）。スピーカーが鳴り出すまでの遅れに合わせる。負なら光らせてから鳴らす。
// 実測で決める（画面と音を一緒に撮ったスローモーション動画で、ずれを測る）
constexpr int kSyncFlashDelayMs = 0;
// 鳴らす時刻の少し前に SPI のロックを取っておき、SD の書き込みや画面全体の描き直しで遅れないようにする
constexpr int kSyncLockLeadMs = 50;

std::atomic<bool> s_sync_active{false};  // 同期信号の画面が出ていて、画面が点いている
std::atomic<bool> s_sync_flash{false};   // 四角が光っている（画面全体を描き直すときに使う）
M5Canvas s_syncv(&M5.Display);           // 時刻の数字（2 色。内部 RAM）

int64_t wall_us() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
}

void draw_sync() {
    status_bar("同期信号");
    struct tm t;
    rtc_clock::local_now(&t);
    char b[24];
    strftime(b, sizeof(b), "%Y-%m-%d", &t);
    text(kFont16, kText, kW / 2, kSyncClockY - 20, b, textdatum_t::top_center);
    strftime(b, sizeof(b), "%H:%M:%S", &t);
    text(&fonts::Font7, kText, kW / 2, kSyncClockY, b, textdatum_t::top_center);
    s_cv.drawRect(kSyncFlashX, kSyncFlashY, kSyncFlashW, kSyncFlashH, kDim);
    if (s_sync_flash.load()) s_cv.fillRect(kSyncFlashX + 2, kSyncFlashY + 2, kSyncFlashW - 4, kSyncFlashH - 4, kText);
    text(kFont12, kDim, kW / 2, kSyncFlashY + kSyncFlashH + 5, "マイクの近くに置き、カメラに写してください",
         textdatum_t::top_center);
    buttons("", "", "戻る");
}

// 以下の 2 つは SPI のロックを持って呼ぶ
void sync_draw_flash(bool on) {
    s_sync_flash.store(on);
    M5.Display.fillRect(kSyncFlashX + 2, kSyncFlashY + 2, kSyncFlashW - 4, kSyncFlashH - 4, on ? kText : kBg);
}

void sync_draw_clock() {
    struct tm t;
    rtc_clock::local_now(&t);
    char b[12];
    strftime(b, sizeof(b), "%H:%M:%S", &t);
    s_syncv.fillScreen(0);
    text(s_syncv, &fonts::Font7, 1, kW / 2, 0, b, textdatum_t::top_center);
    s_syncv.pushSprite(0, kSyncClockY);
}

void sync_task(void*) {
    for (;;) {
        if (!s_sync_active.load()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        // 次の秒の変わり目に、時刻の数字を描き換え、鳴らす秒なら鳴らして光らせる
        int64_t next_us = (wall_us() / 1000000 + 1) * 1000000;
        int phase = static_cast<int>((next_us / 1000000) % kSyncPeriodSec);
        bool mark = phase == 0;
        bool pip = phase >= kSyncFirstPipSec;
        int64_t wait_us = next_us - ((mark || pip) ? kSyncLockLeadMs * 1000 : 0) - wall_us();
        if (wait_us >= 1000) vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));
        {
            SpiLockGuard lock;
            while (next_us - wall_us() > 1500) vTaskDelay(1);
            while (wall_us() < next_us) {
            }
            // 画面を離れた・消灯したあとは描かない（離れる側がロックを取って後始末する）
            if (!s_sync_active.load()) continue;
            if (mark || pip) {
                auto show = [&] {
                    if (mark) M5.Display.invertDisplay(true);
                    else sync_draw_flash(true);
                };
                auto beep = [&] {
                    sound::play(mark ? sound::Cue::SyncMark : sound::Cue::SyncPip);
                    taskYIELD();  // 同じ優先度の音のタスクに、すぐ鳴らさせる
                };
                if (kSyncFlashDelayMs < 0) {
                    show();
                    vTaskDelay(pdMS_TO_TICKS(-kSyncFlashDelayMs));
                    beep();
                } else {
                    beep();
                    if (kSyncFlashDelayMs > 0) vTaskDelay(pdMS_TO_TICKS(kSyncFlashDelayMs));
                    show();
                }
            }
            sync_draw_clock();
        }
        if (mark || pip) {
            vTaskDelay(pdMS_TO_TICKS(mark ? sound::kSyncMarkMs : sound::kSyncPipMs));
            SpiLockGuard lock;
            if (mark) M5.Display.invertDisplay(false);
            else if (s_sync_active.load()) sync_draw_flash(false);
            else s_sync_flash.store(false);
        }
    }
}

// 同期信号の画面に入った・出た（消灯・点灯を含む）
void sync_start() {
    sound::set_loud(true);
    M5.Display.setBrightness(kSyncBrightness);
    s_sync_active.store(true);
}

void sync_stop() {
    s_sync_active.store(false);
    {
        SpiLockGuard lock;
        M5.Display.invertDisplay(false);
    }
    s_sync_flash.store(false);
    sound::set_loud(false);
}

void render_full() {
    auto compose = [] {
        s_cv.fillScreen(kBg);
        switch (s_screen) {
            case Screen::Main: draw_main(); break;
            case Screen::Menu: draw_menu(); break;
            case Screen::Devices: draw_devices(); break;
            case Screen::Log: draw_log(); break;
            case Screen::Sync: draw_sync(); break;
        }
        draw_toast();
    };
    if (s_screen == Screen::Sync) {
        // 同期信号の画面は sync_task がロックを持って描き換えるので、組み立てから送るまでをロックの中で行い、
        // 光っている四角を古い絵で消さないようにする
        SpiLockGuard lock;
        compose();
        s_cv.pushSprite(0, 0);
        return;
    }
    compose();
    SpiLockGuard lock;
    s_cv.pushSprite(0, 0);
}

// メーターの部分だけを描き直す
void render_vu() {
    if (s_screen != Screen::Main) return;
    s_vu.fillScreen(kBg);
    draw_leds(s_vu, 0, 0);
    SpiLockGuard lock;
    s_vu.pushSprite(kVuX, kVuY);
}

// ---- 操作 ----------------------------------------------------------------

void go(Screen s) {
    if (s_screen == Screen::Devices && s != Screen::Devices) bt_ag::stop_discovery();
    if (s == Screen::Devices) bt_ag::start_discovery();
    if (s == Screen::Devices && s_screen != Screen::Devices) {
        s_devices_back = s_screen;  // メニューから来たらメニューへ、メイン画面から来たらメイン画面へ戻る
        s_dev_scroll = 0;
    }
    if (s == Screen::Menu && s_screen == Screen::Main) s_menu_scroll = 0;  // ログ画面などから戻ったときは位置を保つ
    if (s == Screen::Log && s_screen != Screen::Log) {
        s_log_offset = 0;
        s_log_seen_total = event_log_total();
        s_log_new = false;
    }
    bool enter_sync = s == Screen::Sync && s_screen != Screen::Sync;
    if (s != Screen::Sync && s_screen == Screen::Sync) {
        sync_stop();
        M5.Display.setBrightness(kBrightness[s_brightness]);
        event_log("同期信号を終了");
    }
    s_screen = s;
    if (enter_sync) {
        event_log("同期信号を開始");
        sync_start();
    }
    static const char* kNames[] = {"main", "menu", "devices", "log", "sync"};
    ESP_LOGI("ui", "screen %s", kNames[static_cast<int>(s)]);
}

void toggle_rec() {
    if (g_state.rec_requested.load()) {
        g_state.rec_requested.store(false);
        sound::play(sound::Cue::RecStop);
        return;
    }
    if (!g_state.sd_mounted.load()) {
        toast("SD カードがありません");
        return;
    }
    // ループバックの声が通話相手に聞こえたり録音に混ざったりしないよう、録音中は止める
    bt_ag::set_loopback(false);
    g_state.rec_started_ms.store(millis32());
    g_state.rec_requested.store(true);
    sound::play(sound::Cue::RecStart);
}

void on_main(int btn, int, int) {
    if (btn == 0) {
        BtState st = g_state.bt_state.load();
        if (st == BtState::Idle) {
            if (!bt_ag::connect_saved()) go(Screen::Devices);
        } else if (st >= BtState::Connecting) {
            bt_ag::disconnect();
        }
    } else if (btn == 1) {
        go(Screen::Menu);
    } else if (btn == 2) {
        toggle_rec();
    }
}

void graceful_shutdown(const char* message = "シャットダウン中です…", bool factory_reset = false);

void on_menu(int btn, int x, int y) {
    if (btn == 2) {
        go(Screen::Main);
        return;
    }
    if (btn >= 0 || y < kBarH || y >= kMenuBottom) return;
    int content_y = y - kRowY0 + s_menu_scroll;  // スクロールした分を足して、どの行かを決める
    if (content_y < 0 || content_y % kRowPitch >= kRowH) return;  // 行と行のすき間
    int row = content_y / kRowPitch;
    BtState st = g_state.bt_state.load();
    switch (row) {
        case kRowPair:
            go(Screen::Devices);
            break;
        case kRowAudio:
            if (st < BtState::Connected) toast("インカムに接続してください");
            else if (st == BtState::AudioOn) bt_ag::audio_close();
            else bt_ag::audio_open();
            break;
        case kRowLoopback:
            if (g_state.loopback.load()) bt_ag::set_loopback(false);
            else if (st != BtState::AudioOn) toast("音声接続中に使えます");
            else if (g_state.rec_requested.load()) toast("録音中はループバックできません");
            else bt_ag::set_loopback(true);
            break;
        case kRowSync:
            go(Screen::Sync);
            break;
        case kRowMicGain: {
            if (st < BtState::Connected) {
                toast("インカムに接続してください");
                break;
            }
            if (!g_state.hf_remote_volume.load()) break;  // 非対応のインカムでは押せない
            int step = x >= kStepMinusX - 6 && x < kStepMinusX + kStepW + 6 ? -1
                       : x >= kStepPlusX - 6                            ? +1
                                                                        : 0;
            if (step == 0) break;
            int gain = g_state.hf_mic_gain.load();
            if (gain < 0) gain = settings::get_mic_gain() >= 0 ? settings::get_mic_gain() : 8;
            bt_ag::set_mic_gain(gain + step);
            break;
        }
        case kRowSplit: {
            if (g_state.rec_requested.load()) break;  // 録音中は変えられない（行に「録音中変更不可」と出している）
            // 押すたびに次の長さへ
            const SplitOption& next = kSplitOptions[(split_index() + 1) % kSplitCount];
            g_state.split_ms.store(next.sec * 1000);
            settings::set_split_sec(next.sec);
            break;
        }
        case kRowLogView:
            go(Screen::Log);
            break;
        case kRowLogSd: {
            bool on = !g_state.log_to_sd.load();
            g_state.log_to_sd.store(on);
            settings::set_log_to_sd(on);
            break;
        }
        case kRowReconnect: {
            bool on = !g_state.reconnect_on_boot.load();
            g_state.reconnect_on_boot.store(on);
            settings::set_reconnect_on_boot(on);
            break;
        }
        case kRowBrightness:
            s_brightness = (s_brightness + 1) % 3;
            M5.Display.setBrightness(kBrightness[s_brightness]);
            break;
        case kRowReset:
            if (g_state.rec_requested.load()) break;  // 録音中は使えない（行に「録音中変更不可」と出している）
            if (reset_armed() && x >= kResetBtnX - 6) {
                s_reset_armed_until = 0;
                graceful_shutdown("設定を初期化しています…", true);  // 戻ってこない（再起動する）
            } else {
                s_reset_armed_until = millis32() + kResetConfirmMs;
            }
            break;
        default:
            break;
    }
}

void on_sync(int btn, int, int) {
    if (btn == 2) go(Screen::Menu);
}

void on_log(int btn, int, int) {
    if (btn == 0) {
        s_log_offset = 0;  // 最新へ
        s_log_new = false;
    } else if (btn == 2) {
        go(Screen::Menu);
    }
}

void on_devices(int btn, int x, int y) {
    if (btn == 0) {
        BtState st = g_state.bt_state.load();
        if (st == BtState::Idle || st == BtState::Discovering) bt_ag::start_discovery();  // 灰色のときは何もしない
        return;
    }
    if (btn == 2) {
        go(s_devices_back);
        return;
    }
    if (btn >= 0 || y < kBarH || y >= kDevBottom) return;
    int cy = y - kDevRowY0 + s_dev_scroll;
    if (cy < 0 || cy % kDevRowPitch >= kDevRowH) return;  // 行と行のすき間
    int row = cy / kDevRowPitch;
    if (row >= s_row_count) return;
    const DeviceRow& r = s_rows[row];

    if (!r.paired) {
        // まだペアリングしていない機器: 行のどこを押してもペアリングする
        bt_ag::connect_to(r.bda, r.name);
        toast("ペアリングしています…");
        go(Screen::Main);
        return;
    }
    if (x >= kDevBtnX) {
        // 「削除」: 押し間違いを防ぐため、もう一度押したら消す
        if (delete_armed(r.bda)) {
            bt_ag::remove_pairing(r.bda);
            s_delete_armed_until = 0;
        } else {
            memcpy(s_delete_armed, r.bda, 6);
            s_delete_armed_until = millis32() + kDeleteConfirmMs;
        }
        return;
    }
    // ペアリング済みの機器の行: その機器につなぐ
    if (r.connected) {
        toast("接続中です");
        return;
    }
    if (g_state.bt_state.load() >= BtState::Connecting) {
        bt_ag::connect_paired(r.bda, r.name);  // 別の機器につながっていれば、まず切る
        toast("切断しました。もう一度押すと接続します");
        return;
    }
    bt_ag::connect_paired(r.bda, r.name);
    toast("接続しています…");
    go(Screen::Main);
}

// 入力があれば true
// ---- 電源ボタン ------------------------------------------------------------

bool s_screen_on = true;

// スマホと同じように、消すときも点けるときもバックライトをフェードさせる
constexpr uint32_t kFadeOffMs = 150;
constexpr uint32_t kFadeOnMs = 100;
constexpr int kFadeSteps = 12;

// Core2 のバックライトは電源 IC（I2C）で調整するので、SPI のロックは要らない
void fade_brightness(int from, int to, uint32_t ms) {
    for (int i = 1; i <= kFadeSteps; i++) {
        M5.Display.setBrightness(from + (to - from) * i / kFadeSteps);
        vTaskDelay(pdMS_TO_TICKS(ms / kFadeSteps));
    }
}

void set_screen(bool on) {
    s_screen_on = on;
    bool sync = s_screen == Screen::Sync;
    int target = sync ? kSyncBrightness : kBrightness[s_brightness];
    if (on) {
        {
            SpiLockGuard lock;
            M5.Display.wakeup();
            M5.Display.setBrightness(0);
        }
        render_full();  // 暗いうちに今の画面を描いておき、古い画面がちらつかないようにする
        fade_brightness(0, target, kFadeOnMs);
        if (sync) sync_start();  // 消灯中は鳴らさない
    } else {
        if (sync) sync_stop();
        fade_brightness(target, 0, kFadeOffMs);
        SpiLockGuard lock;
        M5.Display.sleep();
    }
}

template <typename Pred>
void wait_until(Pred done, uint32_t timeout_ms) {
    uint32_t t0 = millis32();
    while (!done() && millis32() - t0 < timeout_ms) vTaskDelay(pdMS_TO_TICKS(20));
}

// 電源ボタンの長押し: 録音を閉じ、SD を外し、Bluetooth を切ってから電源を切る。
// 電源 IC は押し始めから 4 秒で強制的に切るが、長押しの知らせは 1 秒で来るので、その間に終える。
// 「シャットダウン中です…」を読めるよう、最低限この時間は表示を残す
constexpr uint32_t kShutdownToastMs = 1000;

// factory_reset: 電源を切る代わりに、NVS をすべて消して再起動する（メニューの「設定の初期化」）
void graceful_shutdown(const char* message, bool factory_reset) {
    if (s_screen == Screen::Sync) go(Screen::Main);  // 同期信号を止めてから（シャットダウンの音と重ねない）
    if (!s_screen_on) set_screen(true);
    // 録音中なら、まずふだんの「停止」と同じ流れで止める（ボタンが「録音」に戻り、ドソが鳴る）
    if (g_state.rec_requested.load()) {
        toggle_rec();
        render_full();
        wait_until([] { return !g_state.recording.load(); }, 1500);  // ファイルを閉じるまで
        wait_until([] { return !sound::busy(); }, 500);                // ドソが鳴り終わるまで
    }

    uint32_t t0 = millis32();
    strlcpy(s_toast, message, sizeof(s_toast));
    s_toast_until = UINT32_MAX;
    render_full();
    event_log(factory_reset ? "設定の初期化を開始" : "シャットダウン開始");
    sound::play(sound::Cue::PowerOff);

    bt_ag::set_loopback(false);
    g_state.rec_requested.store(false);
    // audio タスクが最後の Ogg ページを書いてファイルを閉じるのを待つ
    wait_until([] { return !g_state.recording.load(); }, 1500);
    bool sd_ok = storage::shutdown(1500);
    if (g_state.bt_state.load() >= BtState::Connecting) {
        bt_ag::disconnect();
        wait_until([] { return g_state.bt_state.load() <= BtState::Idle; }, 800);
    }
    ESP_LOGI("ui", "shutdown: sd %s", sd_ok ? "ok" : "timeout");
    // 保存は終わっている。トーストを読めるよう、表示を出してから kShutdownToastMs は待って切る
    uint32_t elapsed = millis32() - t0;
    if (elapsed < kShutdownToastMs) vTaskDelay(pdMS_TO_TICKS(kShutdownToastMs - elapsed));
    if (factory_reset) {
        // Bluetooth を切ったあとに消すので、切断のときにスタックが書いた鍵も残らない
        settings::erase_all();
        esp_restart();
    }
    M5.Power.powerOff();
}

bool handle_input() {
    if (M5.BtnPWR.wasHold()) {
        graceful_shutdown();
        return true;
    }
    if (M5.BtnPWR.wasClicked()) {
        set_screen(!s_screen_on);
        return true;
    }
    if (!s_screen_on) return false;  // 消灯中はほかの操作を受け付けない

    int btn = -1;
    if (M5.BtnA.wasClicked()) btn = 0;
    else if (M5.BtnB.wasClicked()) btn = 1;
    else if (M5.BtnC.wasClicked()) btn = 2;

    int x = -1, y = -1;
    if (btn < 0) {
        auto t = M5.Touch.getDetail();
        // ログ画面では、なぞった量を行に換算して動かす（画面全体は描き直さず、文字の部分だけ）
        if (!t.isPressed()) s_log_drag_px = 0;
        if (s_screen == Screen::Log && t.isPressed() && (t.isFlicking() || t.isDragging()) &&
            t.base_y >= kLogY0 && t.base_y < kBtnY) {
            int before = s_log_offset;
            s_log_drag_px += t.deltaY();
            while (s_log_drag_px >= kLogLineH) {  // 下へなぞる = 古い行を見る
                s_log_offset++;
                s_log_drag_px -= kLogLineH;
            }
            while (s_log_drag_px <= -kLogLineH) {
                s_log_offset--;
                s_log_drag_px += kLogLineH;
            }
            sync_log_view();
            if (s_log_offset != before) s_log_fast_due = true;
            return false;
        }
        // 機器の画面も、なぞった分だけ上下にスクロールする
        if (s_screen == Screen::Devices && t.isPressed() && (t.isFlicking() || t.isDragging()) &&
            t.base_y >= kBarH && t.base_y < kDevBottom) {
            int before = s_dev_scroll;
            s_dev_scroll = std::clamp(s_dev_scroll - t.deltaY(), 0, dev_scroll_max());
            return s_dev_scroll != before;
        }
        // メニューでは、なぞった分だけ上下にスクロールする（なぞり終わりはタップとして扱われない）
        if (s_screen == Screen::Menu && t.isPressed() && (t.isFlicking() || t.isDragging()) &&
            t.base_y >= kBarH && t.base_y < kMenuBottom) {
            int before = s_menu_scroll;
            s_menu_scroll = std::clamp(s_menu_scroll - t.deltaY(), 0, menu_scroll_max());
            return s_menu_scroll != before;
        }
        if (!t.wasClicked() || t.y >= kH) return false;  // 液晶の下（丸印）は BtnA/B/C として上で扱った
        x = t.x;
        y = t.y;
        // 画面下端に描いたボタンの文字を押した場合も、同じボタンとして扱う
        if (y >= kBtnY) btn = std::clamp(x / 107, 0, 2);
    }
    switch (s_screen) {
        case Screen::Main: on_main(btn, x, y); break;
        case Screen::Menu: on_menu(btn, x, y); break;
        case Screen::Devices: on_devices(btn, x, y); break;
        case Screen::Log: on_log(btn, x, y); break;
        case Screen::Sync: on_sync(btn, x, y); break;
    }
    return true;
}

// 消灯中に電源ボタンを確かめる間隔。電源 IC が押されたことを覚えているので、押し逃さない
constexpr uint32_t kScreenOffPollMs = 50;
constexpr uint32_t kScreenOffPowerMs = 2000;  // 消灯中に電池を確かめる間隔
constexpr const char* kBatteryEmptyMessage = "電池切れ：シャットダウン中です…";

void ui_task(void*) {
    uint32_t last_full = 0;
    uint32_t last_vu = 0;
    uint32_t last_log_fast = 0;
    uint32_t last_power_off = 0;
    for (;;) {
        if (!s_screen_on) {
            uint32_t t = millis32();
            if (t - last_power_off >= kScreenOffPowerMs) {
                read_power();
                last_power_off = t;
                if (s_battery_empty) graceful_shutdown(kBatteryEmptyMessage);
            }
            // 消灯中はタッチ（3 つのボタンを含む）を読まず、電源ボタンだけを電源 IC に問い合わせる（電池は上で 2 秒ごと）
            // （getKeyState: 0 = なし、2 = 短押し、それ以外 = 長押し）
            uint8_t key = M5.Power.getKeyState();
            if (key == 2) {
                set_screen(true);
                M5.update();  // 消灯中にたまったタッチの状態を読み捨てる
                render_full();
                last_full = millis32();
            } else if (key != 0) {
                graceful_shutdown();
            }
            vTaskDelay(pdMS_TO_TICKS(kScreenOffPollMs));
            continue;
        }

        M5.update();
        bool input = handle_input();
        uint32_t now = millis32();
        update_stats(now);
        if (s_battery_empty) graceful_shutdown(kBatteryEmptyMessage);

        uint32_t vu_interval = g_state.recording.load() ? kVuIntervalRecMs : kVuIntervalIdleMs;
        bool vu_due = now - last_vu >= vu_interval;
        if (vu_due) {
            update_meters(now);
            last_vu = now;
        }
        if (!s_screen_on) {
            // いま消灯した（次の周回から上の消灯中の処理になる）
        } else if (input || now - last_full >= kFullIntervalMs) {
            render_full();
            last_full = now;
        } else if (s_screen == Screen::Log && s_log_fast_due && now - last_log_fast >= kLogFastIntervalMs) {
            render_log_fast();
            s_log_fast_due = false;
            last_log_fast = now;
        } else if (vu_due) {
            render_vu();
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

}  // namespace

void start() {
    M5.Display.setBrightness(kBrightness[s_brightness]);
    s_cv.setColorDepth(16);
    s_cv.setPsram(true);
    s_cv.createSprite(kW, kH);
    s_vu.setColorDepth(16);
    s_vu.setPsram(false);  // 小さいので内部 RAM に置き、速く送る
    s_vu.createSprite(kVuW, kVuH);
    s_logv.setColorDepth(1);  // 2 色（0 = 背景、1 = 文字）で約 7KB
    s_logv.setPsram(false);
    s_logv.createSprite(kW, kLogAreaH);
    s_logv.setPaletteColor(0, 0x000000u);
    s_logv.setPaletteColor(1, 0x00BEFFu);  // kAccent と同じ水色
    s_syncv.setColorDepth(1);  // 2 色で約 2KB
    s_syncv.setPsram(false);
    s_syncv.createSprite(kW, kSyncClockH);
    s_syncv.setPaletteColor(0, 0x000000u);
    s_syncv.setPaletteColor(1, 0xFFFFFFu);
    s_meter_ms = millis32();
    xTaskCreatePinnedToCore(ui_task, "ui", 8192, nullptr, 2, nullptr, 1);
    // 同期信号の時刻を守るため、録音処理（10）より上、音のタスクと同じ優先度にする
    xTaskCreatePinnedToCore(sync_task, "sync", 4096, nullptr, 11, nullptr, 1);
}

}  // namespace ui
