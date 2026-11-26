// 時計。起動時に RTC（BM8563）から本体の時刻を合わせ、以後は本体の時刻（time()）を使う。
// RTC は日本時間で持つ。
//
// 時刻合わせ: メニューの「時刻合わせ」で合わせる（set）。
// SD カードの直下に settime.txt を置き、中に「2026-11-20 10:30:00」の形で書いて起動しても合わせられる。
// 読み込んだら RTC に書き込み、ファイル名を settime.done に変える。
#pragma once

#include <ctime>

namespace rtc_clock {

constexpr int kMinValidYear = 2025;  // これより前の年は、RTC が止まっていた（電池切れなど）とみなす

void init();  // M5.begin と SD のマウントの後に呼ぶ
void local_now(struct tm* out);
// 日本時間で時刻を合わせる（RTC と本体の時計の両方）。曜日は日付から求める
void set(const struct tm& local);
// 「2026-11-20 10:30:00」（秒は省ける）を読む。範囲外なら false
bool parse_datetime(const char* s, struct tm* out);

}  // namespace rtc_clock
