// mSBC の受信で、Bluedroid が欠損補間（PLC）で作った音を見分ける（msbc_plc.c）
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// いまアプリに渡される PCM（受信のコールバック）が、補間で作ったものか。読んだら下ろす。
// 受信のコールバックの中で呼ぶ。CVSD では復号しないので、いつも false
bool msbc_plc_take(void);

#ifdef __cplusplus
}
#endif
