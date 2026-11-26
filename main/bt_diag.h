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

#ifdef __cplusplus
}
#endif
