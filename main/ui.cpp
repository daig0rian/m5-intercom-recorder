#include "ui.h"

#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "M5Unified.h"
#include "bt_ag.h"
#include "bt_diag.h"
#include "esp_system.h"
#include "esp_vfs.h"
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

enum class Screen { Main, Menu, Devices, Log, Sync, Clock };
constexpr const char* kScreenNames[] = {"main", "menu", "devices", "log", "sync", "clock"};  // ログとシリアルのコマンド

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
    BtState st = g_state.bt_state.load();
    if (bt_ag::reconnecting() && st <= BtState::Connecting) return "つなぎ直し中…";
    switch (st) {
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

// SD カードの形（右上の角を落とした四角）。入っていなければ中抜きに斜線。満杯なら赤
void sd_icon(int x, int y, bool mounted, bool full) {
    constexpr int w = 14, h = 18, cut = 5;
    if (mounted) {
        s_cv.fillRect(x, y, w, h, full ? kRed : kText);
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

// SD の残りで録れる時間を出すときの色の境目（分）。録音中は、これを切ったときに注意の音で知らせる
constexpr uint32_t kSdLowMinutes = 60;
constexpr uint32_t kSdVeryLowMinutes = 15;

// ヘッダー右側の SD（と、残りで録れる時間）・電池・時刻
void header_icons() {
    char b[24];
    struct tm t;
    rtc_clock::local_now(&t);
    snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
    text(kFont16, kText, kW - 4, 3, b, textdatum_t::top_right);
    battery_icon(kW - 4 - 40 - 6 - 39, 3);
    constexpr int kSdX = kW - 4 - 40 - 6 - 39 - 10 - 14;
    bool mounted = g_state.sd_mounted.load();
    bool full = mounted && g_state.sd_full.load();
    sd_icon(kSdX, 2, mounted, full);
    if (mounted && !full) {
        uint32_t min = storage::minutes_left(g_state.sample_rate.load());
        if (min >= 60) snprintf(b, sizeof(b), "残り%lu時間", (unsigned long)(min / 60));
        else snprintf(b, sizeof(b), "残り%lu分", (unsigned long)min);
        uint16_t color = min < kSdVeryLowMinutes ? kRed : min < kSdLowMinutes ? kOrange : kText;
        text(kFont12, color, kSdX - 4, 5, b, textdatum_t::top_right);
    }
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
// 録音中（消灯）に電池が切れるまで測った電圧の下がり方から、% が録音できる残り時間に比例するよう表で換算する
// （消費電力はほぼ一定なので、% は「残りのエネルギー（電圧×電流の積算）」の割合にしている）。
// 電圧の下がり方は電池ごとに違う（同じ残りでも最大 90mV ほど違った）ので、試したどの電池でも、
// 表示が実際の残りより多くならないようにする（余裕があると思ったら無かった、を避ける）。
// 0% は 3.5V とし、電源 IC が自分で切る 3.0V までに数分の余裕を残す（3.3V で切るより、録音できる時間が約 2% 減る）。
// 電圧は流れる電流で下がる（画面を点けると 10mV ほど）ので、電圧をならしてから換算し、電池で動いている間は
// 表示を増やさない。
struct LevelPoint {
    int16_t mv;
    int8_t pct;
};
// 電圧の高い順。点の間は直線で結ぶ。録音中（消灯、約 125〜145mA）に、満充電（充電の表示が消えてから）から
// 0% まで測った 3 本の電池（500mAh の 2 本と、使い込んだ 390mAh）の電圧で、各 % で 3 本のうち最も高い電圧を採る。
// 満充電で USB を抜くと約 4.12V から始まる。表示が実際の残りより少なく出るのは、500mAh で最大 15 ポイント、
// 使い込んだ 390mAh で最大 35 ポイント（内部抵抗が大きく、同じ残りでも電圧が低く出るため）。
// 下の端は電圧が急に下がるので点を細かくする。
constexpr LevelPoint kAxp192Levels[] = {
    {4116, 100}, {4089, 99}, {4069, 97}, {4058, 95}, {4035, 90}, {4010, 85}, {3989, 80}, {3975, 75}, {3954, 70},
    {3922, 65},  {3897, 60}, {3862, 55}, {3839, 50}, {3827, 45}, {3812, 40}, {3798, 35}, {3780, 30}, {3766, 25},
    {3748, 20},  {3719, 15}, {3683, 10}, {3644, 7},  {3631, 5},  {3586, 3},  {3555, 2},  {3520, 1},  {3500, 0},
};
// USB を抜いた直後は、充電で上がっていた電圧が数秒で下がる（4.09V → 3.95V を 3 秒）。
// その高い電圧からならし始めないよう、電池で動き始めてから kFollowMs の間は、ならさずに電圧に従う
constexpr uint32_t kFollowMs = 10 * 1000;
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
    } else if (now - g.since_ms < kFollowMs) {
        g.mv = mv;
        g.last_ms = now;
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
constexpr bool kBatteryTestLog = false;
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
        text(kFont16, kOrange, 6, 3, g_state.sd_unreadable.load() ? "SDカードが読めません" : "SDカードがありません");
    } else if (g_state.sd_full.load()) {
        text(kFont16, kRed, 6, 3, "SDカードがいっぱいです");
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
    snprintf(b, sizeof(b), "書込 %luMB  エラー %lu  空き %lu.%luGB", (unsigned long)(g_state.written_kb.load() / 1024),
             (unsigned long)(g_state.write_errors.load() + g_state.write_queue_drops.load()),
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
    if (bt_ag::reconnecting()) a = "BT切断";  // つなぎ直しの間は、押すとやめる
    else if (st == BtState::Idle) a = peer[0] ? "BT接続" : "機器検索";
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
    kRowClock,
    kRowMicGain,
    kRowSplit,
    kRowLogView,
    kRowLogSd,
    kRowReconnect,
    kRowBrightness,
    kRowSdFormat,
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
uint32_t s_format_armed_until = 0;  // 「SD カードの初期化」も同じように 2 回押す

bool reset_armed() { return millis32() < s_reset_armed_until; }
bool format_armed() { return millis32() < s_format_armed_until; }

// 初期化できるカードが入っている（読めるカードか、応えるが FAT として読めないカード）
bool sd_card_present() { return g_state.sd_mounted.load() || g_state.sd_unreadable.load(); }

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
                text(kFont16, kText, 16, yc, "映像同期", textdatum_t::middle_left);
                text(kFont12, kDim, kRowNoteX, yc, "映像と音を合わせる", textdatum_t::middle_left);
                break;
            case kRowClock: {
                // 録音中は変えられない（ファイル名や区切りの時刻がどうなるか分かりにくくなるため）
                bool rec = g_state.rec_requested.load();
                text(kFont16, rec ? kDim : kText, 16, yc, "時刻合わせ", textdatum_t::middle_left);
                if (rec) {
                    text(kFont12, kDim, kRowNoteX, yc, "録音中変更不可", textdatum_t::middle_left);
                } else {
                    struct tm t;
                    rtc_clock::local_now(&t);
                    char b[24];
                    strftime(b, sizeof(b), "%Y-%m-%d %H:%M", &t);
                    text(kFont12, kDim, kRowNoteX, yc, b, textdatum_t::middle_left);
                }
                break;
            }
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
            case kRowSdFormat: {
                // 録音中と、カードが入っていないときは使えない
                bool rec = g_state.rec_requested.load();
                bool usable = !rec && sd_card_present();
                bool armed = usable && format_armed();
                text(kFont16, usable ? kText : kDim, 16, yc, "SDカード初期化", textdatum_t::middle_left);
                const char* note = rec                            ? "録音中変更不可"
                                   : !sd_card_present()           ? "カードなし"
                                   : g_state.sd_unreadable.load() ? "読めないカード"
                                                                  : "全部消えます";
                // 見出しが長いので、添え書きは少し右に置く（右のボタンにかからない幅）
                text(kFont12, kDim, kRowNoteX + 14, yc, note, textdatum_t::middle_left);
                s_cv.fillRoundRect(kResetBtnX, yc - 12, kResetBtnW, 25, 4, !usable ? 0x2945 : armed ? kRed : 0x4208);
                text(kFont12, usable ? kText : kDim, kResetBtnX + kResetBtnW / 2, yc, armed ? "初期化する?" : "初期化",
                     textdatum_t::middle_center);
                break;
            }
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

// ---- 映像同期 --------------------------------------------------------------
// 映像と録音を合わせるための画面。15 秒ごと（毎分 0・15・30・45 秒）に、時報と同じ並びで鳴らす。
// 12・13・14 秒に予報音（ぴ）を鳴らし、鳴っている間は真ん中の四角を反対の色にする。
// 15 秒ちょうどに正報音（ぷ）を鳴らし、鳴り始めと同時に四角の白と黒を入れ替える（次の正報音まで保つ）。
// スピーカーの音をインカムのマイクに拾わせ、画面をカメラに写す。正報音の鳴り始めと四角が入れ替わるコマで、映像と音を合わせる。
// 鳴らす時刻と描き換える時刻は sync_task が決め、画面の時刻の数字と四角は SPI のロックを持って直接描き換える。

constexpr int kSyncPeriodSec = 15;
constexpr int kSyncFirstPipSec = 12;  // 15 秒周期の中で、予報音を鳴らし始める秒
constexpr int kSyncClockY = 47;       // 時刻の数字（Font7、高さ 48）
constexpr int kSyncClockH = 48;
constexpr int kSyncFlashX = 40;  // 白黒が入れ替わる四角
constexpr int kSyncFlashY = 103;
constexpr int kSyncFlashW = kW - 80;
constexpr int kSyncFlashH = 88;
constexpr uint8_t kSyncBrightness = 255;  // カメラに写りやすいよう、この画面の間は一番明るくする
// 鳴らす指示から四角を描き換えるまでの時間（ms）。スピーカーが鳴り出すまでの遅れに合わせる。負なら描き換えてから鳴らす。
// 画面と音を一緒に撮ったスローモーション動画で、0 で四角の入れ替わりと音の鳴り始めが合っていた
constexpr int kSyncFlashDelayMs = 0;
// 鳴らす時刻の少し前に SPI のロックを取っておき、SD の書き込みや画面全体の描き直しで遅れないようにする
constexpr int kSyncLockLeadMs = 50;

std::atomic<bool> s_sync_active{false};  // 映像同期の画面が出ていて、画面が点いている
// 四角の色（画面全体を描き直すときにも使う）。地の色は「ぷ」のたびに白と黒を入れ替え、次の「ぷ」まで保つ。
// 動画を編集する人は、コマ送りでこの入れ替わり（エッジ）を見つけ、音の立ち上がりに重ねる。
// 「ぴ」が鳴っている間は、地の色の反対にする
std::atomic<bool> s_sync_white{false};  // 地の色が白
std::atomic<bool> s_sync_pip{false};    // 「ぴ」で反対の色にしている
uint16_t sync_box_color() { return s_sync_white.load() != s_sync_pip.load() ? kText : kBg; }
M5Canvas s_syncv(&M5.Display);           // 時刻の数字（2 色。内部 RAM）

int64_t wall_us() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
}

void draw_sync() {
    status_bar("映像同期");
    struct tm t;
    rtc_clock::local_now(&t);
    char b[24];
    strftime(b, sizeof(b), "%Y-%m-%d", &t);
    text(kFont16, kText, kW / 2, kSyncClockY - 20, b, textdatum_t::top_center);
    strftime(b, sizeof(b), "%H:%M:%S", &t);
    text(&fonts::Font7, kText, kW / 2, kSyncClockY, b, textdatum_t::top_center);
    s_cv.drawRect(kSyncFlashX, kSyncFlashY, kSyncFlashW, kSyncFlashH, kDim);
    s_cv.fillRect(kSyncFlashX + 2, kSyncFlashY + 2, kSyncFlashW - 4, kSyncFlashH - 4, sync_box_color());
    text(kFont12, kDim, kW / 2, kSyncFlashY + kSyncFlashH + 5, "マイクの近くに置き、カメラに写してください",
         textdatum_t::top_center);
    buttons("", "", "戻る");
}

// 以下の 2 つは SPI のロックを持って呼ぶ
void sync_draw_box() {
    M5.Display.fillRect(kSyncFlashX + 2, kSyncFlashY + 2, kSyncFlashW - 4, kSyncFlashH - 4, sync_box_color());
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
        // 次の秒の変わり目に、時刻の数字を描き換え、鳴らす秒なら鳴らして四角を描き換える
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
                    if (mark) s_sync_white.store(!s_sync_white.load());
                    else s_sync_pip.store(true);
                    sync_draw_box();
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
        if (pip) {
            // 予報音が鳴り終わったら、四角を地の色に戻す（正報音で入れ替えた色は、次の正報音まで保つ）
            vTaskDelay(pdMS_TO_TICKS(sound::kSyncPipMs));
            SpiLockGuard lock;
            s_sync_pip.store(false);
            if (s_sync_active.load()) sync_draw_box();
        }
    }
}

// 映像同期の画面に入った・出た（消灯・点灯を含む）
void sync_start() {
    sound::set_loud(true);
    M5.Display.setBrightness(kSyncBrightness);
    s_sync_active.store(true);
}

void sync_stop() {
    {
        SpiLockGuard lock;  // 描き換えの途中で止めない
        s_sync_active.store(false);
        s_sync_pip.store(false);
    }
    sound::set_loud(false);
}

// ---- 時刻合わせ ------------------------------------------------------------
// 年・月・日・時・分を＋−で選び、時刻が 0 秒ちょうどになったときに「合わせる」を押す。
// 押した瞬間をその分の 0 秒にするので、秒を合わせる操作は要らない（ずれは押す速さの分で済む）。
// 開いたときは、いまの時刻の次の分を入れておく（少しずれているだけなら、分を少し直せば済む）。

void go(Screen s);

enum ClockField { kClkYear, kClkMonth, kClkDay, kClkHour, kClkMinute, kClkFields };
constexpr int kClkColX[kClkFields] = {46, 112, 166, 238, 292};  // 各欄の真ん中
constexpr const char* kClkLabel[kClkFields] = {"年", "月", "日", "時", "分"};
constexpr int kClkLabelY = 52;
constexpr int kClkPlusY = 68;    // ＋の上端
constexpr int kClkValueY = 111;  // 数字の真ん中
constexpr int kClkMinusY = 130;  // −の上端
constexpr int kClkBtnW = 46;
constexpr int kClkBtnH = 26;
int s_clk[kClkFields] = {};

int days_in_month(int y, int m) {
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : kDays[m - 1];
}

void clock_open() {
    time_t next = time(nullptr) + 60;
    struct tm t;
    localtime_r(&next, &t);
    s_clk[kClkYear] = std::max(t.tm_year + 1900, rtc_clock::kMinValidYear);
    s_clk[kClkMonth] = t.tm_mon + 1;
    s_clk[kClkDay] = t.tm_mday;
    s_clk[kClkHour] = t.tm_hour;
    s_clk[kClkMinute] = t.tm_min;
}

void clock_step(int field, int step) {
    auto wrap = [](int v, int lo, int hi) { return v < lo ? hi : v > hi ? lo : v; };
    int& v = s_clk[field];
    switch (field) {
        case kClkYear: v = std::clamp(v + step, rtc_clock::kMinValidYear, 2099); break;
        case kClkMonth: v = wrap(v + step, 1, 12); break;
        case kClkDay: v = wrap(v + step, 1, days_in_month(s_clk[kClkYear], s_clk[kClkMonth])); break;
        case kClkHour: v = wrap(v + step, 0, 23); break;
        case kClkMinute: v = wrap(v + step, 0, 59); break;
    }
    // 年や月を変えて、その月にない日になったら月末にする
    s_clk[kClkDay] = std::min(s_clk[kClkDay], days_in_month(s_clk[kClkYear], s_clk[kClkMonth]));
}

void draw_clock() {
    status_bar("時刻合わせ");
    struct tm now;
    rtc_clock::local_now(&now);
    char b[48];
    strftime(b, sizeof(b), "いまの時刻  %Y-%m-%d %H:%M:%S", &now);
    text(kFont16, kText, kW / 2, 28, b, textdatum_t::top_center);
    for (int i = 0; i < kClkFields; i++) {
        int x = kClkColX[i];
        text(kFont12, kDim, x, kClkLabelY, kClkLabel[i], textdatum_t::middle_center);
        s_cv.fillRoundRect(x - kClkBtnW / 2, kClkPlusY, kClkBtnW, kClkBtnH, 4, 0x4208);
        text(kFont16, kText, x, kClkPlusY + kClkBtnH / 2, "＋", textdatum_t::middle_center);
        snprintf(b, sizeof(b), i == kClkYear ? "%04d" : "%02d", s_clk[i]);
        text(&fonts::Font4, kText, x, kClkValueY, b, textdatum_t::middle_center);
        s_cv.fillRoundRect(x - kClkBtnW / 2, kClkMinusY, kClkBtnW, kClkBtnH, 4, 0x4208);
        text(kFont16, kText, x, kClkMinusY + kClkBtnH / 2, "−", textdatum_t::middle_center);
    }
    text(&fonts::Font4, kText, (kClkColX[kClkHour] + kClkColX[kClkMinute]) / 2, kClkValueY, ":",
         textdatum_t::middle_center);
    text(kFont12, kText, kW / 2, 170, "時刻が 0 秒ちょうどになったときに", textdatum_t::top_center);
    text(kFont12, kText, kW / 2, 187, "「合わせる」を押してください", textdatum_t::top_center);
    buttons("", "合わせる", "戻る");
}

// 時刻を合わせる。録音中は変えられない（ファイル名や区切りの時刻がどうなるか分かりにくくなるため）。
// 行えなかったときは理由を返す（画面ではトーストに出す。以下の操作も同じ）
const char* set_clock(const struct tm& local) {
    if (g_state.rec_requested.load()) return "録音中は時刻を変えられません";
    time_t before = time(nullptr);
    struct tm t = local;
    t.tm_isdst = 0;
    struct tm tmp = t;
    time_t after = mktime(&tmp);
    rtc_clock::set(t);
    // どれだけずれていたかも残す（正なら、それまで遅れていた）
    long diff = static_cast<long>(after - before);
    char was[40];
    if (diff == 0) {
        snprintf(was, sizeof(was), "ずれなし");
    } else if (diff > -86400 && diff < 86400) {
        snprintf(was, sizeof(was), "%ld 秒%s", diff > 0 ? diff : -diff, diff > 0 ? "遅れていた" : "進んでいた");
    } else {
        struct tm old;
        localtime_r(&before, &old);
        strftime(was, sizeof(was), "前 %Y-%m-%d %H:%M", &old);
    }
    event_log("時刻合わせ %04d-%02d-%02d %02d:%02d:%02d（%s）", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
              t.tm_min, t.tm_sec, was);
    return nullptr;
}

// 「合わせる」: 押した瞬間を、選んだ分の 0 秒にする
void clock_apply() {
    struct tm t = {};
    t.tm_year = s_clk[kClkYear] - 1900;
    t.tm_mon = s_clk[kClkMonth] - 1;
    t.tm_mday = s_clk[kClkDay];
    t.tm_hour = s_clk[kClkHour];
    t.tm_min = s_clk[kClkMinute];
    if (const char* err = set_clock(t)) {  // 録音中は開けないが、念のため
        toast(err);
        return;
    }
    toast("時刻を合わせました");
    go(Screen::Menu);
}

void on_clock(int btn, int x, int y) {
    if (btn == 2) {
        go(Screen::Menu);
        return;
    }
    if (btn == 1) {
        clock_apply();
        return;
    }
    if (btn >= 0) return;
    int step = y >= kClkPlusY - 6 && y < kClkPlusY + kClkBtnH + 6     ? +1
               : y >= kClkMinusY - 6 && y < kClkMinusY + kClkBtnH + 6 ? -1
                                                                      : 0;
    if (step == 0) return;
    for (int i = 0; i < kClkFields; i++) {
        if (x >= kClkColX[i] - kClkBtnW / 2 - 3 && x <= kClkColX[i] + kClkBtnW / 2 + 3) {
            clock_step(i, step);
            return;
        }
    }
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
            case Screen::Clock: draw_clock(); break;
        }
        draw_toast();
    };
    if (s_screen == Screen::Sync) {
        // 映像同期の画面は sync_task がロックを持って描き換えるので、組み立てから送るまでをロックの中で行い、
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
        event_log("映像同期を終了");
    }
    s_screen = s;
    if (enter_sync) {
        event_log("映像同期を開始");
        sync_start();
    }
    ESP_LOGI("ui", "screen %s", kScreenNames[static_cast<int>(s)]);
}

// ---- SD の空き ----------------------------------------------------------------
// 満杯の手前になったら録音を止めて知らせる。録音中に残りが 1 時間・15 分を切ったら知らせる

bool s_sd_full_prev = false;
int s_sd_warned = 0;  // 録音中に知らせた段階（0 = まだ、1 = 1 時間、2 = 15 分）

int sd_low_stage(uint32_t min) { return min < kSdVeryLowMinutes ? 2 : min < kSdLowMinutes ? 1 : 0; }

// 定期的に呼ぶ。消していた画面をつけたほうがよいとき（満杯で止めた・満杯のカードが挿さった）は true
bool check_sd_space() {
    bool full = g_state.sd_mounted.load() && g_state.sd_full.load();
    bool became_full = full && !s_sd_full_prev;
    s_sd_full_prev = full;
    if (became_full) {
        if (g_state.rec_requested.load()) {
            g_state.rec_requested.store(false);
            event_log("SDカードがいっぱいなので録音を止めました");
            sound::play(sound::Cue::RecStop);
        }
        // ログは 1 行 96 バイトまで・画面の幅に収まるよう、2 行に分ける
        event_log("SDカードがいっぱいです（空き %luMB）", (unsigned long)g_state.sd_free_mb.load());
        event_log("古い録音を PC で消すか、初期化してください");
        sound::play(sound::Cue::Warning);
        toast("SDカードがいっぱいです");
        return true;
    }
    if (full || !g_state.sd_mounted.load() || !g_state.rec_requested.load()) return false;

    uint32_t min = storage::minutes_left(g_state.sample_rate.load());
    int stage = sd_low_stage(min);
    if (stage > s_sd_warned) {
        event_log("SDカードの残りが %lu 分を切りました", (unsigned long)(stage == 2 ? kSdVeryLowMinutes : kSdLowMinutes));
        sound::play(sound::Cue::Warning);
        s_sd_warned = stage;
    } else if (stage < s_sd_warned) {
        // カードの空きを測り直して増えたとき。境目の前後で何度も鳴らないよう、5 分の余裕をもって戻す
        uint32_t border = s_sd_warned == 2 ? kSdVeryLowMinutes : kSdLowMinutes;
        if (min >= border + 5) s_sd_warned = stage;
    }
    return false;
}

// ---- 操作の中身 ----------------------------------------------------------------
// 画面のボタンとシリアルのコマンドの両方から呼ぶ。行えなかったときは理由を返す（画面ではトーストに出す）

void show(const char* err) {
    if (err) toast(err);
}

void stop_rec() {
    if (!g_state.rec_requested.load()) return;
    g_state.rec_requested.store(false);
    sound::play(sound::Cue::RecStop);
}

const char* start_rec() {
    if (g_state.rec_requested.load()) return nullptr;
    if (!g_state.sd_mounted.load()) {
        return g_state.sd_unreadable.load() ? "SD カードが読めません" : "SD カードがありません";
    }
    if (g_state.sd_full.load()) {
        sound::play(sound::Cue::Warning);
        return "SDカードがいっぱいです";
    }
    // ループバックの声が通話相手に聞こえたり録音に混ざったりしないよう、録音中は止める
    bt_ag::set_loopback(false);
    g_state.rec_started_ms.store(millis32());
    g_state.rec_requested.store(true);
    sound::play(sound::Cue::RecStart);
    // 始めたときに残りが少なければ、いまの残りを出す（その段階の注意の音は、もう鳴らさない）
    uint32_t min = storage::minutes_left(g_state.sample_rate.load());
    s_sd_warned = sd_low_stage(min);
    if (s_sd_warned > 0) {
        char b[48];
        snprintf(b, sizeof(b), "SDカードの残りは約 %lu 分です", (unsigned long)min);
        toast(b);
    }
    return nullptr;
}

constexpr const char* kNoPeer = "接続先がありません（機器を探してください）";

// 最後に接続した機器につなぐ
const char* bt_connect() {
    BtState st = g_state.bt_state.load();
    if (bt_ag::reconnecting()) return "つなぎ直し中です";
    if (st == BtState::Discovering) return "機器を検索中です";
    if (st == BtState::Idle && !bt_ag::connect_saved()) return kNoPeer;
    return nullptr;  // つながっている・つなぎに行っているときは何もしない
}

// 切る。録音中のつなぎ直しもやめる
void bt_disconnect() {
    if (bt_ag::reconnecting() || g_state.bt_state.load() >= BtState::Connecting) bt_ag::disconnect();
}

const char* set_audio(bool on) {
    if (g_state.bt_state.load() < BtState::Connected) return "インカムに接続してください";
    if (on) bt_ag::audio_open();
    else bt_ag::audio_close();
    return nullptr;
}

const char* set_loopback(bool on) {
    if (!on) {
        bt_ag::set_loopback(false);
        return nullptr;
    }
    if (g_state.bt_state.load() != BtState::AudioOn) return "音声接続中に使えます";
    if (g_state.rec_requested.load()) return "録音中はループバックできません";
    bt_ag::set_loopback(true);
    return nullptr;
}

const char* set_mic_gain(int gain) {
    if (g_state.bt_state.load() < BtState::Connected) return "インカムに接続してください";
    if (!g_state.hf_remote_volume.load()) return "非対応機器です";
    bt_ag::set_mic_gain(gain);
    return nullptr;
}

// 録音中は変えられない（いまのファイルや次の区切りがどうなるか分かりにくくなるため）
const char* set_split(uint32_t sec) {
    if (g_state.rec_requested.load()) return "録音中は変えられません";
    for (const SplitOption& o : kSplitOptions) {
        if (o.sec != sec) continue;
        g_state.split_ms.store(sec * 1000);
        settings::set_split_sec(sec);
        return nullptr;
    }
    return "その長さは選べません";
}

void set_log_to_sd(bool on) {
    g_state.log_to_sd.store(on);
    settings::set_log_to_sd(on);
}

void set_reconnect_on_boot(bool on) {
    g_state.reconnect_on_boot.store(on);
    settings::set_reconnect_on_boot(on);
}

void set_brightness(int level) {
    s_brightness = std::clamp(level, 0, 2);
    M5.Display.setBrightness(kBrightness[s_brightness]);
}

// 機器の一覧の行を選んだとき。結果をトーストの文言で返し、メイン画面へ戻るなら *done を立てる
const char* select_device(const DeviceRow& r, bool* done) {
    *done = false;
    if (!r.paired) {
        bt_ag::connect_to(r.bda, r.name);  // まだペアリングしていない機器: ペアリングする
        *done = true;
        return "ペアリングしています…";
    }
    if (r.connected) return "接続中です";
    bool busy = g_state.bt_state.load() >= BtState::Connecting;
    bt_ag::connect_paired(r.bda, r.name);  // 別の機器につながっていれば、まず切る
    if (busy) return "切断しました。もう一度押すと接続します";
    *done = true;
    return "接続しています…";
}

void on_main(int btn, int, int) {
    if (btn == 0) {
        BtState st = g_state.bt_state.load();
        if (bt_ag::reconnecting() || st >= BtState::Connecting) bt_disconnect();
        else if (st == BtState::Idle && bt_connect() == kNoPeer) go(Screen::Devices);
    } else if (btn == 1) {
        go(Screen::Menu);
    } else if (btn == 2) {
        if (g_state.rec_requested.load()) stop_rec();
        else show(start_rec());
    }
}

void graceful_shutdown(const char* message = "シャットダウン中です…", bool factory_reset = false);

// 大きいカードでは FAT の表を書くのに時間がかかる。その間、液晶と同じバスを使うので画面は止まる
constexpr uint32_t kFormatTimeoutMs = 10 * 60 * 1000;

void format_sd() {
    strlcpy(s_toast, "SD カードを初期化しています…", sizeof(s_toast));
    s_toast_until = UINT32_MAX;
    render_full();
    event_log("SD カードの初期化を開始");
    uint32_t t0 = millis32();
    bool ok = storage::format_card(kFormatTimeoutMs);
    uint32_t sec = (millis32() - t0) / 1000;
    if (ok) event_log("SD カードを初期化しました（%lu 秒）", (unsigned long)sec);
    else event_log("SD カードを初期化できませんでした（%lu 秒）", (unsigned long)sec);
    toast(ok ? "SD カードを初期化しました" : "初期化できませんでした");
}

// 録音中と、カードが入っていないときは使えない
const char* format_card() {
    if (g_state.rec_requested.load()) return "録音中は初期化できません";
    if (!sd_card_present()) return "SD カードを入れてください";
    format_sd();
    return nullptr;
}

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
            show(set_audio(st != BtState::AudioOn));
            break;
        case kRowLoopback:
            show(set_loopback(!g_state.loopback.load()));
            break;
        case kRowSync:
            go(Screen::Sync);
            break;
        case kRowClock:
            if (g_state.rec_requested.load()) break;  // 録音中は変えられない（行に「録音中変更不可」と出している）
            clock_open();
            go(Screen::Clock);
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
            show(set_mic_gain(gain + step));
            break;
        }
        case kRowSplit:
            if (g_state.rec_requested.load()) break;  // 録音中は変えられない（行に「録音中変更不可」と出している）
            show(set_split(kSplitOptions[(split_index() + 1) % kSplitCount].sec));  // 押すたびに次の長さへ
            break;
        case kRowLogView:
            go(Screen::Log);
            break;
        case kRowLogSd:
            set_log_to_sd(!g_state.log_to_sd.load());
            break;
        case kRowReconnect:
            set_reconnect_on_boot(!g_state.reconnect_on_boot.load());
            break;
        case kRowBrightness:
            set_brightness((s_brightness + 1) % 3);
            break;
        case kRowSdFormat:
            if (g_state.rec_requested.load()) break;  // 録音中は使えない（行に「録音中変更不可」と出している）
            if (!sd_card_present()) {
                toast("SD カードを入れてください");
                break;
            }
            if (format_armed() && x >= kResetBtnX - 6) {
                s_format_armed_until = 0;
                show(format_card());
            } else {
                s_format_armed_until = millis32() + kResetConfirmMs;
            }
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

    if (r.paired && x >= kDevBtnX) {
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
    // まだペアリングしていない機器は、行のどこを押してもペアリングする。ペアリング済みの機器は、その機器につなぐ
    bool done;
    toast(select_device(r, &done));
    if (done) go(Screen::Main);
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
    if (s_screen == Screen::Sync) go(Screen::Main);  // 映像同期を止めてから（シャットダウンの音と重ねない）
    if (!s_screen_on) set_screen(true);
    // 録音中なら、まずふだんの「停止」と同じ流れで止める（ボタンが「録音」に戻り、ドソが鳴る）
    if (g_state.rec_requested.load()) {
        stop_rec();
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
    // audio タスクが最後のデータを書いてファイルを閉じるのを待つ
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

// いまの画面で押す。btn: 下の 3 つのボタン（0〜2。-1 = 画面の中を押した）。x, y: 押した位置
void press(int btn, int x, int y) {
    switch (s_screen) {
        case Screen::Main: on_main(btn, x, y); break;
        case Screen::Menu: on_menu(btn, x, y); break;
        case Screen::Devices: on_devices(btn, x, y); break;
        case Screen::Log: on_log(btn, x, y); break;
        case Screen::Sync: on_sync(btn, x, y); break;
        case Screen::Clock: on_clock(btn, x, y); break;
    }
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
    press(btn, x, y);
    return true;
}

// ---- シリアルからの操作 ----------------------------------------------------------
// 試験を自動で進めるため、画面の操作と同じことを USB のシリアルから 1 行のコマンドで行う（help で一覧）。
// 返事は「@」で始まる行にして、ログの行と見分けられるようにする。コマンドごとに最後に 1 行、
// 「@ok <コマンド>」か「@err <コマンド>: <理由>」を返す（その前に、結果の行が付くことがある）。
// ドライバーを入れず、UI タスクから標準入力を待たずに読む。受信の FIFO は 128 バイトなので、
// 1 行ずつ送って返事を待てばあふれない（消灯中に読む間隔の 50ms で、115200bps なら約 580 バイト届く）。

constexpr const char* kBtNames[] = {"off", "idle", "discovering", "connecting", "connected", "audio"};

constexpr const char* kHelp[] = {
    "status                  状態を 1 行で返す",
    "rec on|off              録音を始める・止める（メイン画面の「録音」「停止」）",
    "bt connect|disconnect   最後に接続した機器につなぐ・切る（メイン画面の左のボタン）",
    "audio on|off            音声接続（メニュー。以下も同じ）",
    "loopback on|off         ループバックモニター",
    "gain <0-15>             マイク感度",
    "split <秒>              ファイル分割（0 30 60 300 900 1800 3600。0 = 分割なし）",
    "logsd on|off            ログを SD に保存",
    "autoconnect on|off      起動時に再接続",
    "bright <0-2>            画面の明るさ（0 = 高）",
    "display on|off          画面を点ける・消す（電源ボタンの短押し）",
    "go <画面>               画面を移る（main menu devices log sync clock。devices で検索も始める）",
    "btn a|b|c               画面の下の左・真ん中・右のボタン（画面が点いているときだけ）",
    "tap <x> <y>             画面を押す（320×240。画面が点いているときだけ）",
    "devices                 機器の画面の行の一覧（番号は上からの順）",
    "select <番号>           機器の行を押す（ペアリング・接続）",
    "unpair <番号>           ペアリングを削除する",
    "time <YYYY-MM-DD HH:MM:SS>  時刻合わせ（受け取った瞬間をその時刻にする）",
    "note <文>               ログに印を残す（SD のログにも残る）",
    "hcilog on|off           Bluetooth のホストとコントローラーのやり取り（HCI）を @hci の行で出す（調べるとき用）",
    "hcicmd <命令> [引数]    HCI のコマンドを直接送る（命令は 4 桁の 16 進。引数は 16 進で、peer は接続先のアドレス、",
    "                        handle は接続のハンドルに置き換える。例: hcicmd 080b peer 01 で Core2 が子機になる）",
    "format yes              SD カードを初期化する",
    "reset yes               設定を初期化する（再起動する）",
    "poweroff                電源を切る（電源ボタンの長押し）",
};

void reply_ok(const char* cmd, const char* note = nullptr) {
    if (note) printf("@ok %s — %s\n", cmd, note);
    else printf("@ok %s\n", cmd);
    fflush(stdout);
}

void reply_err(const char* cmd, const char* why) {
    printf("@err %s: %s\n", cmd, why);
    fflush(stdout);
}

void reply(const char* cmd, const char* err) {
    if (err) reply_err(cmd, err);
    else reply_ok(cmd);
}

void print_status() {
    char peer[40], file[48], now[24], drop[12];
    state_get_peer_name(peer, sizeof(peer));
    state_get_file_name(file, sizeof(file));
    struct tm t;
    rtc_clock::local_now(&t);
    strftime(now, sizeof(now), "%Y-%m-%dT%H:%M:%S", &t);
    int d = g_state.pkt_drop_x10.load();
    if (d >= 0) snprintf(drop, sizeof(drop), "%d.%d", d / 10, d % 10);
    else strlcpy(drop, "-", sizeof(drop));
    Codec codec = g_state.codec.load();
    const char* sd = !g_state.sd_mounted.load() ? (g_state.sd_unreadable.load() ? "unreadable" : "none")
                     : g_state.sd_full.load()   ? "full"
                                                : "ok";
    int role = bt_ag::link_role();
    printf("@status time=%s uptime_s=%lu screen=%s display=%s bt=%s role=%s reconnecting=%d peer=\"%s\" codec=%s "
           "audio_wanted=%d loopback=%d drop=%s rec=%d recording=%d file=\"%s\" rec_s=%lu overflows=%lu sd=%s "
           "free_mb=%lu left_min=%lu write_err=%lu battery=%d charging=%d usb=%d gain=%d split=%lu logsd=%d "
           "autoconnect=%d\n",
           now, (unsigned long)(millis32() / 1000), kScreenNames[static_cast<int>(s_screen)], s_screen_on ? "on" : "off",
           kBtNames[static_cast<int>(g_state.bt_state.load())], role == 0 ? "central" : role == 1 ? "peripheral" : "-",
           bt_ag::reconnecting(), peer,
           codec == Codec::Msbc ? "msbc" : codec == Codec::Cvsd ? "cvsd" : "none", bt_ag::audio_wanted(),
           g_state.loopback.load(), drop, g_state.rec_requested.load(), g_state.recording.load(), file,
           (unsigned long)(g_state.rec_audio_ms.load() / 1000), (unsigned long)g_state.ring_overflows.load(), sd,
           (unsigned long)g_state.sd_free_mb.load(), (unsigned long)storage::minutes_left(g_state.sample_rate.load()),
           (unsigned long)(g_state.write_errors.load() + g_state.write_queue_drops.load()), s_battery, s_charging,
           usb_power(), g_state.hf_mic_gain.load(), (unsigned long)(g_state.split_ms.load() / 1000),
           g_state.log_to_sd.load(), g_state.reconnect_on_boot.load());
}

void print_devices() {
    build_device_rows();
    for (int i = 0; i < s_row_count; i++) {
        const DeviceRow& r = s_rows[i];
        if (r.paired) {
            printf("@dev %d paired \"%s\"%s\n", i, r.name, !r.last ? "" : r.connected ? " connected" : " last");
        } else {
            printf("@dev %d found \"%s\" rssi=%d%s\n", i, r.name, r.rssi, r.is_audio ? " audio" : "");
        }
    }
}

bool parse_on_off(const char* s, bool* on) {
    if (!s) return false;
    if (strcmp(s, "on") == 0) *on = true;
    else if (strcmp(s, "off") == 0) *on = false;
    else return false;
    return true;
}

bool parse_int(const char* s, int* out) {
    if (!s || !*s) return false;
    char* end;
    long v = strtol(s, &end, 10);
    if (*end) return false;
    *out = static_cast<int>(v);
    return true;
}

// 空白で区切る（区切りを '\0' に書き換える）。最大 max 個
int split_words(char* s, char* words[], int max) {
    int n = 0;
    while (n < max) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        words[n++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = '\0';
    }
    return n;
}

// 機器の画面の行の番号から、その行を取る（一覧は、そのときの検索の結果から作り直す）
const DeviceRow* device_row(const char* index) {
    int i;
    if (!parse_int(index, &i)) return nullptr;
    build_device_rows();
    return i >= 0 && i < s_row_count ? &s_rows[i] : nullptr;
}

// hcicmd の引数を読む。16 進の並び（空白は読み飛ばす）。「peer」は接続先のアドレス（HCI の並び = 下の桁から）、
// 「handle」は接続のハンドル（2 バイト）に置き換える。読めなければ -1
int parse_hci_params(const char* s, uint8_t* out, int max) {
    auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    int n = 0;
    while (*s) {
        if (*s == ' ') {
            s++;
        } else if (strncmp(s, "peer", 4) == 0 || strncmp(s, "handle", 6) == 0) {
            bool peer = s[0] == 'p';
            uint8_t bda[6];
            if (!bt_ag::peer_address(bda)) return -1;
            if (peer) {
                if (n + 6 > max) return -1;
                for (int i = 0; i < 6; i++) out[n++] = bda[5 - i];
                s += 4;
            } else {
                int h = bt_diag_acl_handle(bda);
                if (h < 0 || n + 2 > max) return -1;
                out[n++] = h & 0xFF;
                out[n++] = h >> 8;
                s += 6;
            }
        } else {
            int hi = hex(s[0]), lo = s[1] ? hex(s[1]) : -1;
            if (hi < 0 || lo < 0 || n >= max) return -1;
            out[n++] = static_cast<uint8_t>(hi << 4 | lo);
            s += 2;
        }
    }
    return n;
}

void run_command(char* line) {
    char cmd[128];
    strlcpy(cmd, line, sizeof(cmd));  // 返事に付ける（line は区切って使う）
    char* w[4] = {};
    int n = split_words(line, w, 4);
    if (n == 0) return;
    auto is = [&](const char* name) { return strcmp(w[0], name) == 0; };
    auto arg_is = [&](const char* s) { return w[1] && strcmp(w[1], s) == 0; };
    // 1 つ目の引数から行の終わりまで（note・time）
    const char* rest = w[1] ? cmd + (w[1] - line) : "";
    bool on;
    int v, x, y;

    if (is("help")) {
        for (const char* h : kHelp) printf("@help %s\n", h);
        reply_ok(cmd);
    } else if (is("status")) {
        print_status();
        reply_ok(cmd);
    } else if (is("rec") && parse_on_off(w[1], &on)) {
        if (on) {
            reply(cmd, start_rec());
        } else {
            stop_rec();
            reply_ok(cmd);
        }
    } else if (is("bt") && arg_is("connect")) {
        reply(cmd, bt_connect());
    } else if (is("bt") && arg_is("disconnect")) {
        bt_disconnect();
        reply_ok(cmd);
    } else if (is("audio") && parse_on_off(w[1], &on)) {
        reply(cmd, set_audio(on));
    } else if (is("loopback") && parse_on_off(w[1], &on)) {
        reply(cmd, set_loopback(on));
    } else if (is("gain") && parse_int(w[1], &v) && v >= 0 && v <= 15) {
        reply(cmd, set_mic_gain(v));
    } else if (is("split") && parse_int(w[1], &v) && v >= 0) {
        reply(cmd, set_split(v));
    } else if (is("logsd") && parse_on_off(w[1], &on)) {
        set_log_to_sd(on);
        reply_ok(cmd);
    } else if (is("autoconnect") && parse_on_off(w[1], &on)) {
        set_reconnect_on_boot(on);
        reply_ok(cmd);
    } else if (is("bright") && parse_int(w[1], &v) && v >= 0 && v <= 2) {
        set_brightness(v);
        reply_ok(cmd);
    } else if (is("display") && parse_on_off(w[1], &on)) {
        if (on != s_screen_on) set_screen(on);
        reply_ok(cmd);
    } else if (is("go") && w[1]) {
        int s = -1;
        for (int i = 0; i < static_cast<int>(sizeof(kScreenNames) / sizeof(kScreenNames[0])); i++) {
            if (strcmp(w[1], kScreenNames[i]) == 0) s = i;
        }
        if (s < 0) {
            reply_err(cmd, "その画面はありません");
        } else if (static_cast<Screen>(s) == Screen::Clock && g_state.rec_requested.load()) {
            reply_err(cmd, "録音中は時刻を変えられません");  // メニューの「時刻合わせ」も、録音中は押せない
        } else {
            if (static_cast<Screen>(s) == Screen::Clock) clock_open();
            go(static_cast<Screen>(s));
            reply_ok(cmd);
        }
    } else if ((is("btn") && w[1] && w[1][0] >= 'a' && w[1][0] <= 'c' && !w[1][1]) ||
               (is("tap") && parse_int(w[1], &x) && parse_int(w[2], &y) && x >= 0 && x < kW && y >= 0 && y < kH)) {
        if (!s_screen_on) {
            reply_err(cmd, "画面が消えています（display on で点けてください）");  // 消灯中は画面の操作を受け付けない
        } else if (is("btn")) {
            press(w[1][0] - 'a', -1, -1);
            reply_ok(cmd);
        } else {
            press(y >= kBtnY ? std::clamp(x / 107, 0, 2) : -1, x, y);
            reply_ok(cmd);
        }
    } else if (is("devices")) {
        print_devices();
        reply_ok(cmd);
    } else if (is("select")) {
        const DeviceRow* r = device_row(w[1]);
        if (!r) {
            reply_err(cmd, "その番号の機器はありません（devices で一覧）");
        } else {
            bool done;
            reply_ok(cmd, select_device(*r, &done));
            if (done && s_screen == Screen::Devices) go(Screen::Main);  // 画面で押したときと同じく、メイン画面へ
        }
    } else if (is("unpair")) {
        const DeviceRow* r = device_row(w[1]);
        if (!r || !r->paired) {
            reply_err(cmd, "その番号のペアリング済みの機器はありません（devices で一覧）");
        } else {
            bt_ag::remove_pairing(r->bda);
            reply_ok(cmd);
        }
    } else if (is("time")) {
        struct tm t;
        if (!rtc_clock::parse_datetime(rest, &t)) reply_err(cmd, "日時は YYYY-MM-DD HH:MM:SS で");
        else reply(cmd, set_clock(t));
    } else if (is("note") && *rest) {
        event_log("[印] %s", rest);
        reply_ok(cmd);
    } else if (is("hcicmd") && w[1]) {
        uint8_t params[64];
        char* end;
        long op = strtol(w[1], &end, 16);
        const char* args = w[2] ? cmd + (w[2] - line) : "";
        int plen = *end || op <= 0 || op > 0xFFFF ? -1 : parse_hci_params(args, params, sizeof(params));
        if (plen < 0) reply_err(cmd, "命令は 4 桁の 16 進、引数は 16 進・peer・handle で");
        else if (!bt_diag_hci_send(static_cast<uint16_t>(op), params, plen)) reply_err(cmd, "前のコマンドがまだ送れていません");
        else reply_ok(cmd);
    } else if (is("hcilog") && parse_on_off(w[1], &on)) {
        bt_diag_hci_log(on);
        if (bt_diag_hci_logging() == on) reply_ok(cmd);
        else reply_err(cmd, "記録の置き場を取れません");
    } else if (is("format") && arg_is("yes")) {
        reply(cmd, format_card());
    } else if (is("reset") && arg_is("yes")) {
        if (g_state.rec_requested.load()) {
            reply_err(cmd, "録音中は初期化できません");
        } else {
            reply_ok(cmd);
            graceful_shutdown("設定を初期化しています…", true);  // 戻ってこない（再起動する）
        }
    } else if (is("poweroff")) {
        reply_ok(cmd);
        graceful_shutdown();  // 戻ってこない
    } else {
        reply_err(cmd, "分からないコマンドです（help で一覧）");
    }
}

char s_line[128];
size_t s_line_len = 0;
bool s_line_long = false;  // 長すぎる行（行の終わりまで捨てる）

// 届いた分を読み、行がそろったら行う。行ったら true
bool poll_serial() {
    bool ran = false;
    char buf[32];
    ssize_t n;
    // read() は IRAM に置かれる関数で、IRAM はほぼ満杯なので、VFS の関数を直接呼ぶ
    while ((n = esp_vfs_read(__getreent(), STDIN_FILENO, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {  // CR も LF に変わって届く（CONFIG_NEWLIB_STDIN_LINE_ENDING_CR）
                s_line[s_line_len] = '\0';
                if (s_line_long) {
                    reply_err("(長すぎる行)", "1 行は 127 バイトまで");
                } else if (s_line_len > 0) {
                    run_command(s_line);
                    ran = true;
                }
                s_line_len = 0;
                s_line_long = false;
            } else if (s_line_len + 1 < sizeof(s_line)) {
                s_line[s_line_len++] = c;
            } else {
                s_line_long = true;
            }
        }
    }
    return ran;
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
            poll_serial();
            if (s_screen_on) continue;  // display on で点けた
            uint32_t t = millis32();
            bool wake = false;
            if (t - last_power_off >= kScreenOffPowerMs) {
                read_power();
                last_power_off = t;
                if (s_battery_empty) graceful_shutdown(kBatteryEmptyMessage);
                wake = check_sd_space();  // 満杯で録音を止めたら、画面をつけて知らせる
            }
            // 消灯中はタッチ（3 つのボタンを含む）を読まず、電源ボタンだけを電源 IC に問い合わせる（電池は上で 2 秒ごと）
            // （getKeyState: 0 = なし、2 = 短押し、それ以外 = 長押し）
            uint8_t key = M5.Power.getKeyState();
            if (key != 0 && key != 2) {
                graceful_shutdown();
            } else if (key == 2 || wake) {
                set_screen(true);
                M5.update();  // 消灯中にたまったタッチの状態を読み捨てる
                render_full();
                last_full = millis32();
            }
            vTaskDelay(pdMS_TO_TICKS(kScreenOffPollMs));
            continue;
        }

        M5.update();
        bool input = handle_input();
        if (poll_serial()) input = true;
        uint32_t now = millis32();
        update_stats(now);
        if (s_battery_empty) graceful_shutdown(kBatteryEmptyMessage);
        if (check_sd_space()) input = true;  // すぐ描き直す

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
    // 映像同期の時刻を守るため、録音処理（10）より上、音のタスクと同じ優先度にする
    xTaskCreatePinnedToCore(sync_task, "sync", 4096, nullptr, 11, nullptr, 1);
}

}  // namespace ui
