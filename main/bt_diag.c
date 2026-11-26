// 調べるための仕掛け。
// - Bluetooth の接続での Core2 の役割（親機 = Central、子機 = Peripheral）を知る。役割が入れ替わったとき
//   （HCI の Role Change）は、シリアルにログを出す。
// - ホスト（Bluedroid）とコントローラーの間のやり取り（HCI）を記録して、シリアルに書き出す（bt_diag_hci_log）。
//   PC で tools/hci_to_btsnoop.py にかけると、Wireshark で開ける。
//
// IDF は書き換えず、リンク時に差し替える（main/CMakeLists.txt の --wrap）。同じファイルの中からの呼び出しは
// 差し替わらないので、IDF を上げたら、btm_acl_role_changed が btu_hcif.c から、esp_vhci_host_send_packet と
// esp_vhci_host_register_callback が esp_bluedroid_hci.c から呼ばれていることを確かめる。
// IRAM はほぼ満杯なので、IRAM に置かれる関数を新しく呼ばない（記録の置き場も自前のリングにしている）。

#include <stdio.h>
#include <string.h>

#include "common/bt_target.h"
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stack/btm_api.h"
#include "bt_diag.h"

static const char* TAG = "bt_diag";

// ---- 役割 ----------------------------------------------------------------------

void __real_btm_acl_role_changed(UINT8 hci_status, BD_ADDR bd_addr, UINT8 new_role);

void __wrap_btm_acl_role_changed(UINT8 hci_status, BD_ADDR bd_addr, UINT8 new_role) {
    if (new_role == BTM_ROLE_UNDEFINED) {
        ESP_LOGW(TAG, "役割の切り替えを頼めなかった (0x%02x)", hci_status);
    } else {
        ESP_LOGW(TAG, "役割の切り替え (0x%02x): Core2 は%s", hci_status,
                 new_role == BTM_ROLE_MASTER ? "親機" : "子機");
    }
    __real_btm_acl_role_changed(hci_status, bd_addr, new_role);
}

int bt_diag_role(const uint8_t* bda) {
    UINT8 role;
    if (BTM_GetRole((UINT8*)bda, &role) != BTM_SUCCESS) return -1;
    return role == BTM_ROLE_MASTER ? 0 : 1;
}

int bt_diag_acl_handle(const uint8_t* bda) {
    UINT16 h = BTM_GetHCIConnHandle((UINT8*)bda, BT_TRANSPORT_BR_EDR);
    return h == 0xFFFF ? -1 : h;
}

// ---- HCI の記録 ------------------------------------------------------------------
// 送る側（ホスト → コントローラー）は Bluedroid のタスク、受ける側はコントローラーのタスクから呼ばれるので、
// そこではリング（PSRAM）に写すだけにし、書き出しは優先度の低いタスクで行う。

#define HCI_TYPE_SCO 3
#define HCI_TYPE_EVT 4
#define HCI_EVT_NOCP 0x13
#define HCI_SLOT_BYTES 96  // 1 件に残す長さ（H4 の種類の 1 バイトを含む）。長いものは切る
#define HCI_SLOTS 1024     // 約 110KB

typedef struct {
    int64_t t_us;
    uint16_t len;  // 元の長さ
    char dir;      // '>' = ホスト → コントローラー、'<' = コントローラー → ホスト
    uint8_t data[HCI_SLOT_BYTES];
} hci_slot_t;

static volatile bool s_hci_on = false;
static hci_slot_t* s_slots = NULL;
static uint32_t s_head = 0;  // 書いた数
static uint32_t s_tail = 0;  // 書き出した数
static uint32_t s_dropped = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_vhci_host_callback_t s_host_cb;  // Bluedroid のコールバック
static esp_vhci_host_callback_t s_wrap_cb;

// 鍵と PIN は記録に残さない（0 で埋める）
static void redact(uint8_t* d, uint16_t kept) {
    uint16_t op = kept >= 3 && d[0] == 1 ? (uint16_t)(d[1] | d[2] << 8) : 0;
    int from = 0, n = 0;
    if (op == 0x040B) from = 10, n = 16;                                   // Link Key Request Reply
    else if (op == 0x040D) from = 10, n = 17;                              // PIN Code Request Reply
    else if (d[0] == HCI_TYPE_EVT && kept >= 2 && d[1] == 0x18) from = 9, n = 16;  // Link Key Notification
    for (int i = from; i < from + n && i < kept; i++) d[i] = 0;
}

static void hci_record(char dir, const uint8_t* data, uint16_t len) {
    if (!s_hci_on || !data || len == 0) return;
    if (data[0] == HCI_TYPE_SCO) return;
    if (data[0] == HCI_TYPE_EVT && len > 1 && data[1] == HCI_EVT_NOCP) return;
    int64_t t = esp_timer_get_time();
    uint16_t kept = len < HCI_SLOT_BYTES ? len : HCI_SLOT_BYTES;
    taskENTER_CRITICAL(&s_mux);
    if (s_head - s_tail >= HCI_SLOTS) {
        s_dropped++;
    } else {
        hci_slot_t* s = &s_slots[s_head % HCI_SLOTS];
        s->t_us = t;
        s->len = len;
        s->dir = dir;
        memcpy(s->data, data, kept);
        redact(s->data, kept);
        s_head++;
    }
    taskEXIT_CRITICAL(&s_mux);
}

// ---- HCI のコマンドを直接送る（調べるとき用） -------------------------------------
// Bluedroid の送信と重ならないよう、Bluedroid が何かを送った直後に、同じタスクから続けて送る。
// 送るものがない間（音声接続がないとき）は、書き出しのタスクから送る。
// Bluedroid は、自分が頼んでいないコマンドの返事を受けると、警告を出して捨てる（hci_layer.c）。

#define HCI_INJECT_FALLBACK_US 200000
static uint8_t s_inj[4 + 255];
static uint16_t s_inj_len = 0;  // 0 = 送るものなし
static int64_t s_inj_since = 0;
static uint8_t s_inj_out[sizeof(s_inj)];

void __real_esp_vhci_host_send_packet(uint8_t* data, uint16_t len);

static void inject_try(void) {
    if (!s_inj_len || !esp_vhci_host_check_send_available()) return;
    taskENTER_CRITICAL(&s_mux);
    uint16_t len = s_inj_len;
    if (len) memcpy(s_inj_out, s_inj, len);
    s_inj_len = 0;
    taskEXIT_CRITICAL(&s_mux);
    if (!len) return;
    hci_record('>', s_inj_out, len);
    __real_esp_vhci_host_send_packet(s_inj_out, len);
}

static bool ensure_started(void);

bool bt_diag_hci_send(uint16_t opcode, const uint8_t* params, uint8_t plen) {
    if (!ensure_started()) return false;  // 送るものがない間は、書き出しのタスクから送る
    bool ok = false;
    taskENTER_CRITICAL(&s_mux);
    if (!s_inj_len) {
        s_inj[0] = 1;  // H4 のコマンド
        s_inj[1] = opcode & 0xFF;
        s_inj[2] = opcode >> 8;
        s_inj[3] = plen;
        memcpy(&s_inj[4], params, plen);
        s_inj_since = esp_timer_get_time();
        s_inj_len = 4 + plen;
        ok = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}

void __wrap_esp_vhci_host_send_packet(uint8_t* data, uint16_t len) {
    hci_record('>', data, len);
    __real_esp_vhci_host_send_packet(data, len);
    inject_try();
}

static int recv_cb(uint8_t* data, uint16_t len) {
    hci_record('<', data, len);
    return s_host_cb.notify_host_recv(data, len);
}

esp_err_t __real_esp_vhci_host_register_callback(const esp_vhci_host_callback_t* callback);

esp_err_t __wrap_esp_vhci_host_register_callback(const esp_vhci_host_callback_t* callback) {
    s_host_cb = *callback;
    s_wrap_cb.notify_host_send_available = callback->notify_host_send_available;
    s_wrap_cb.notify_host_recv = recv_cb;
    return __real_esp_vhci_host_register_callback(&s_wrap_cb);
}

// 1 件を「@hci <起動からの秒> <向き> <元の長さ> <16 進>」の 1 行で書く
static void hci_print_task(void* arg) {
    static char line[48 + HCI_SLOT_BYTES * 2];
    hci_slot_t s;
    for (;;) {
        bool got = false;
        uint32_t dropped;
        taskENTER_CRITICAL(&s_mux);
        if (s_tail != s_head) {
            s = s_slots[s_tail % HCI_SLOTS];
            s_tail++;
            got = true;
        }
        dropped = s_dropped;
        s_dropped = 0;
        taskEXIT_CRITICAL(&s_mux);
        if (dropped) printf("@hci 記録があふれて %lu 件を捨てた\n", (unsigned long)dropped);
        if (s_inj_len && esp_timer_get_time() - s_inj_since > HCI_INJECT_FALLBACK_US) inject_try();
        if (!got) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        int n = snprintf(line, sizeof(line), "@hci %lld.%06lld %c %u ", (long long)(s.t_us / 1000000),
                         (long long)(s.t_us % 1000000), s.dir, (unsigned)s.len);
        int kept = s.len < HCI_SLOT_BYTES ? s.len : HCI_SLOT_BYTES;
        static const char kHex[] = "0123456789abcdef";
        for (int i = 0; i < kept; i++) {
            line[n++] = kHex[s.data[i] >> 4];
            line[n++] = kHex[s.data[i] & 0xF];
        }
        line[n++] = '\n';
        fwrite(line, 1, n, stdout);
    }
}

// 記録の置き場と書き出しのタスクを用意する（初めて使うときに 1 度だけ）
static bool ensure_started(void) {
    if (s_slots) return true;
    s_slots = heap_caps_malloc(sizeof(hci_slot_t) * HCI_SLOTS, MALLOC_CAP_SPIRAM);
    if (!s_slots) {
        ESP_LOGE(TAG, "HCI の記録の置き場を取れない");
        return false;
    }
    xTaskCreatePinnedToCore(hci_print_task, "hci_log", 3072, NULL, 1, NULL, 1);
    return true;
}

void bt_diag_hci_log(bool on) {
    if (on && !ensure_started()) return;
    s_hci_on = on;
}

bool bt_diag_hci_logging(void) { return s_hci_on; }
