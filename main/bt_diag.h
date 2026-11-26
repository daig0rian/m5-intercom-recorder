// 調べるための仕掛け（bt_diag.c）
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 接続での Core2 の役割。0 = 親機、1 = 子機、-1 = つながっていない
int bt_diag_role(const uint8_t* bda);
// 接続（ACL）のハンドル。-1 = つながっていない
int bt_diag_acl_handle(const uint8_t* bda);

// Bluetooth のホスト（Bluedroid）とコントローラーの間のやり取り（HCI）を、シリアルに「@hci」の行で書き出す。
// 音声のデータ（SCO）と、送り終えたパケットの知らせ（Number of Completed Packets）は除く
void bt_diag_hci_log(bool on);
bool bt_diag_hci_logging(void);

// HCI のコマンドを 1 つ、Bluedroid を通さずにコントローラーへ送る（調べるとき用）。前のがまだ送れていなければ false
bool bt_diag_hci_send(uint16_t opcode, const uint8_t* params, uint8_t plen);

// 音声のパケット（SCO）と mSBC の復号を数えて、10 秒ごとにシリアルに「@sco」の行で書き出す。
// 1 件ずつの記録も直前の十数秒分を残しておき、bt_diag_sco_dump を呼んだ時刻の前 5 秒・後 0.5 秒を
// 「@scod」の行で書き出す（受信の見張りが音の異常を見つけたときに呼ぶ。30 秒に 1 回まで）
void bt_diag_sco_log(bool on);
bool bt_diag_sco_logging(void);
void bt_diag_sco_dump(const char* why);
// mSBC を 1 回復号した（msbc_plc.c から呼ぶ）。used は読んだ長さ、status は OI_STATUS、plc は補間のための無音のフレームか
void bt_diag_decoded(const uint8_t* in, uint32_t in_len, uint32_t used, int status, bool plc);

#ifdef __cplusplus
}
#endif
