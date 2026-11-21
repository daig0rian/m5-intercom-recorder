// 時計。起動時に RTC（BM8563）から本体の時刻を合わせ、以後は本体の時刻（time()）を使う。
// RTC は日本時間で持つ。
//
// 時刻合わせ: SD カードの直下に settime.txt を置き、中に「2026-11-20 10:30:00」の形で書いて起動する。
// 読み込んだら RTC に書き込み、ファイル名を settime.done に変える。
#pragma once

#include <ctime>

namespace rtc_clock {

void init();  // M5.begin と SD のマウントの後に呼ぶ
void local_now(struct tm* out);

}  // namespace rtc_clock
