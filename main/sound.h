// 操作音と同期信号の音（Core2 の内蔵スピーカー）。操作音は、操作を受け付けたことを音で確かめるためのもの。
// 波形は矩形波。操作音は 1 音 30ms を tone() で順に並べて鳴らす。
#pragma once

namespace sound {

enum class Cue {
    PowerOn,   // ド ミ ソ
    PowerOff,  // ソ ミ ド
    RecStart,  // ソ ド
    RecStop,   // ド ソ
    SyncPip,   // 同期信号の予報音（440Hz 60ms）
    SyncMark,  // 同期信号の正報音（880Hz 0.5 秒）
};
constexpr int kSyncPipMs = 60;
constexpr int kSyncMarkMs = 500;

void init();  // M5.begin のあとに呼ぶ
void play(Cue cue);  // 鳴らし終わるのを待たない
bool busy();         // 鳴らしている途中か（まだ鳴っていないものを含む）
void set_loud(bool on);  // 同期信号の画面の間は、マイクに拾わせるため一番大きくする

}  // namespace sound
