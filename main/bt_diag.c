// 調べるための仕掛け。
// - Bluetooth の接続での Core2 の役割（親機 = Central、子機 = Peripheral）を知る。役割が入れ替わったとき
//   （HCI の Role Change）は、シリアルにログを出す。
// - ホスト（Bluedroid）とコントローラーの間のやり取り（HCI）を記録して、シリアルに書き出す（bt_diag_hci_log）。
//   PC で tools/hci_log.py にかけると、名前を付けた一覧か、Wireshark で開ける形にできる。
// - 音声のパケット（SCO）と mSBC の復号を数えて書き出す（bt_diag_sco_log）。復号は msbc_plc.c の差し替えから知らされる。
//
// IDF は書き換えず、リンク時に差し替える（main/CMakeLists.txt の --wrap）。同じファイルの中からの呼び出しは
// 差し替わらないので、IDF を上げたら、btm_acl_role_changed が btu_hcif.c から、esp_vhci_host_send_packet と
// esp_vhci_host_register_callback が esp_bluedroid_hci.c から、bta_ag_sco_co_open が bta_ag_sco.c から
// 呼ばれていることを確かめる。
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
#include "oi_codec_sbc.h"
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

// ---- 音声のパケット（SCO）と mSBC の復号 ----------------------------------------------
// 音声接続が開いたまま音が壊れる・届かなくなるとき、どこで壊れているかを見るためのもの。
// Bluedroid は、パケットの区切りが mSBC のフレームの区切り（頭に H2 ヘッダー 01 x8 と同期の印 AD）と
// 合っている前提で、パケットを（30 バイトのときは 2 つずつ）復号器に渡す。区切りがずれると、復号器は
// 途中で見つけた同期の印から読もうとして「データが足りない」で終わり、音を何も出さない（欠損補間もしない）。
// - パケット：届いた印（正しい・誤りあり・受信なし・一部欠け）、長さ、H2 ヘッダーの位置、連番の飛び
// - 復号：結果と、渡されたデータの中の H2 ヘッダーの位置
// を 10 秒ごとに数えて「@sco」の行で書き出す。1 件ずつの記録もリング（PSRAM）に残し、
// bt_diag_sco_dump で、頼まれた時刻の前 5 秒・後 0.5 秒を「@scod」の行で書き出す。

#define SCO_EVS 8192                       // 1 件 16 バイト。いちばん多いとき（1 秒 530 件）でも 15 秒分
#define SCO_SUMMARY_US 10000000LL
#define SCO_DUMP_BEFORE_US 5000000LL
#define SCO_DUMP_AFTER_US 500000LL
#define SCO_DUMP_MIN_GAP_US 30000000LL     // 書き出しは 30 秒に 1 回まで（間を空けて書くので、1 回に最大 15 秒ほどかかる）
#define H2_NONE 0xFF

enum { DEC_OK, DEC_NOSYNC, DEC_HDR, DEC_BODY, DEC_AUDIO, DEC_CRC, DEC_INVAL, DEC_OTHER, DEC_N };
static const char* const kDecNames[DEC_N] = {"ok", "nosync", "hdr", "body", "audio", "crc", "inval", "other"};

typedef struct {
    int64_t t_us;
    char kind;        // 'P' = パケット、'D' = 復号
    uint8_t a;        // P: 届いた印（0 = 正しい、1 = 誤りあり、2 = 受信なし、3 = 一部欠け）。D: 結果（DEC_*）
    uint8_t len;      // P: 長さ。D: 渡された長さ
    uint8_t h2;       // H2 ヘッダーの位置（H2_NONE = なし）
    uint8_t b[4];     // P: 先頭の 4 バイト。D: b[0] = 読んだ長さ、b[1] = 1 なら欠損補間のための復号
} sco_ev_t;

typedef struct {
    uint32_t pkts, st[4], len60, len30, lenx;
    uint32_t h2_head;  // 先頭に H2 ヘッダーがあるパケット
    uint32_t h2_mid;   // H2 ヘッダーが先頭以外にあるパケット
    uint32_t sn_skip;  // H2 ヘッダーの連番が飛んだ
    int64_t gap_max_us;
    uint32_t dec[DEC_N];
    uint32_t plc;         // 欠損補間（壊れたフレームの代わり）のための復号
    uint32_t dec_h2_mid;  // 復号器に渡されたデータの先頭に H2 ヘッダーがない（区切りがずれている）
} sco_stats_t;

static volatile bool s_sco_on = false;
static sco_ev_t* s_sco_evs = NULL;
static sco_ev_t* s_sco_snap = NULL;  // 書き出す窓の写し
static uint32_t s_sco_head = 0;
static sco_stats_t s_sco_st;
static portMUX_TYPE s_sco_mux = portMUX_INITIALIZER_UNLOCKED;
static uint16_t s_sco_handle = 0xFFFF;
static int64_t s_sco_last_us = 0;
static int s_sco_sn = -1;
static volatile int64_t s_dump_at = 0;  // 0 = 頼まれていない
static int64_t s_dump_last = 0;
static char s_dump_why[24];

// p[0..n) の中で、H2 ヘッダー（01 08/38/c8/f8）と同期の印（AD）が続くところ。sn に連番（0〜3）
static uint8_t find_h2(const uint8_t* p, int n, int* sn) {
    for (int i = 0; i + 2 < n; i++) {
        if (p[i] != 0x01 || (p[i + 1] & 0x0F) != 0x08 || p[i + 2] != 0xAD) continue;
        uint8_t hi = p[i + 1] >> 4;
        int s = hi == 0x0 ? 0 : hi == 0x3 ? 1 : hi == 0xC ? 2 : hi == 0xF ? 3 : -1;
        if (s < 0) continue;
        if (sn) *sn = s;
        return (uint8_t)i;
    }
    return H2_NONE;
}

static void sco_push(const sco_ev_t* e) {
    s_sco_evs[s_sco_head % SCO_EVS] = *e;
    s_sco_head++;
}

// コントローラーから届いた音声のパケット（H4 の種類の 1 バイトを含む）
static void sco_record_packet(const uint8_t* data, uint16_t len) {
    if (!s_sco_on || len < 4) return;
    uint16_t hs = (uint16_t)(data[1] | data[2] << 8);
    uint16_t handle = hs & 0x0FFF;
    uint8_t st = (hs >> 12) & 0x3;
    uint8_t plen = data[3];
    if (plen > len - 4) plen = (uint8_t)(len - 4);
    const uint8_t* p = data + 4;
    int sn = -1;
    uint8_t h2 = find_h2(p, plen, &sn);
    sco_ev_t e = {.t_us = esp_timer_get_time(), .kind = 'P', .a = st, .len = plen, .h2 = h2};
    for (int i = 0; i < 4 && i < plen; i++) e.b[i] = p[i];

    taskENTER_CRITICAL(&s_sco_mux);
    sco_stats_t* s = &s_sco_st;
    if (handle != s_sco_handle) {
        s_sco_handle = handle;
        s_sco_sn = -1;
    } else if (e.t_us - s_sco_last_us > s->gap_max_us) {
        s->gap_max_us = e.t_us - s_sco_last_us;
    }
    s_sco_last_us = e.t_us;
    s->pkts++;
    s->st[st]++;
    if (plen == 60) s->len60++;
    else if (plen == 30) s->len30++;
    else s->lenx++;
    if (h2 == 0) s->h2_head++;
    else if (h2 != H2_NONE) s->h2_mid++;
    if (h2 != H2_NONE) {
        if (s_sco_sn >= 0 && sn != ((s_sco_sn + 1) & 3)) s->sn_skip++;
        s_sco_sn = sn;
    }
    sco_push(&e);
    taskEXIT_CRITICAL(&s_sco_mux);
}

// mSBC を 1 回復号した（msbc_plc.c の差し替えから呼ばれる）
void bt_diag_decoded(const uint8_t* in, uint32_t in_len, uint32_t used, int status, bool plc) {
    if (!s_sco_on) return;
    uint8_t h2 = plc ? H2_NONE : find_h2(in, in_len > 255 ? 255 : (int)in_len, NULL);
    OI_STATUS r = (OI_STATUS)status;
    int d = r == OI_OK                              ? DEC_OK
            : r == OI_CODEC_SBC_NO_SYNCWORD           ? DEC_NOSYNC
            : r == OI_CODEC_SBC_NOT_ENOUGH_HEADER_DATA ? DEC_HDR
            : r == OI_CODEC_SBC_NOT_ENOUGH_BODY_DATA  ? DEC_BODY
            : r == OI_CODEC_SBC_NOT_ENOUGH_AUDIO_DATA ? DEC_AUDIO
            : r == OI_CODEC_SBC_CHECKSUM_MISMATCH     ? DEC_CRC
            : r == OI_STATUS_INVALID_PARAMETERS       ? DEC_INVAL
                                                      : DEC_OTHER;
    sco_ev_t e = {.t_us = esp_timer_get_time(), .kind = 'D', .a = (uint8_t)d,
                  .len = (uint8_t)(in_len > 255 ? 255 : in_len), .h2 = h2,
                  .b = {(uint8_t)(used > 255 ? 255 : used), plc ? 1 : 0}};
    taskENTER_CRITICAL(&s_sco_mux);
    if (plc) {
        s_sco_st.plc++;
    } else {
        s_sco_st.dec[d]++;
        if (h2 != 0) s_sco_st.dec_h2_mid++;
    }
    sco_push(&e);
    taskEXIT_CRITICAL(&s_sco_mux);
}

// 音声接続が開いて、Bluedroid が受け渡しを始めるとき（パケットの長さが分かる）
void __real_bta_ag_sco_co_open(UINT16 handle, tBTM_SCO_AIR_MODE_TYPE air_mode, UINT8 inout_pkt_size, UINT16 event);

void __wrap_bta_ag_sco_co_open(UINT16 handle, tBTM_SCO_AIR_MODE_TYPE air_mode, UINT8 inout_pkt_size, UINT16 event) {
    ESP_LOGI(TAG, "音声の受け渡しを始める: %s、パケット %u バイト",
             air_mode == BTM_SCO_AIR_MODE_TRANSPNT ? "mSBC" : air_mode == BTM_SCO_AIR_MODE_CVSD ? "CVSD" : "?",
             (unsigned)inout_pkt_size);
    __real_bta_ag_sco_co_open(handle, air_mode, inout_pkt_size, event);
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
    if (data && len && data[0] == HCI_TYPE_SCO) sco_record_packet(data, len);
    else hci_record('<', data, len);
    return s_host_cb.notify_host_recv(data, len);
}

esp_err_t __real_esp_vhci_host_register_callback(const esp_vhci_host_callback_t* callback);

esp_err_t __wrap_esp_vhci_host_register_callback(const esp_vhci_host_callback_t* callback) {
    s_host_cb = *callback;
    s_wrap_cb.notify_host_send_available = callback->notify_host_send_available;
    s_wrap_cb.notify_host_recv = recv_cb;
    return __real_esp_vhci_host_register_callback(&s_wrap_cb);
}

// ---- 音声のパケットと復号の書き出し（書き出しのタスクから） ------------------------------

static int64_t s_sum_since = 0;

static unsigned long ul(uint32_t v) { return (unsigned long)v; }

// 1 行をシリアルに書き、その行を送るのにかかる時間（115200bps で 1 バイト約 87µs）の 2 倍ほど休む。
// 休まずに書き続けると、送信バッファ（128 バイト）の空きを待って回り続けるあいだ、Bluetooth の受信が乱れる
// （4.5 秒書き続けたとき、受信パケットの最大 91% に誤りが付いた）
static void paced_write(const char* s, int len, size_t cap) {
    if (len <= 0) return;
    if ((size_t)len >= cap) len = (int)cap - 1;
    fwrite(s, 1, len, stdout);
    vTaskDelay(pdMS_TO_TICKS(len / 5 + 1));
}
#define PACED_LINE(buf, ...) paced_write((buf), snprintf((buf), sizeof(buf), __VA_ARGS__), sizeof(buf))

// 10 秒ごとに、数えた結果を 1 行で書く。
// 「@sco <起動からの秒> <窓の長さ>s pkt=<パケット> ok/err/none/part=<届いた印ごと> len60/len30/lenx=<長さごと>
//  h2=<先頭に H2> h2mid=<途中に H2> snskip=<連番の飛び> gapmax=<最大の間隔> | dec <結果ごと> plc=<欠損補間>
//  h2mid=<復号器に渡されたデータの先頭に H2 がない>」
static void sco_print_summary(int64_t now) {
    if (now - s_sum_since < SCO_SUMMARY_US) return;
    sco_stats_t s;
    taskENTER_CRITICAL(&s_sco_mux);
    s = s_sco_st;
    s_sco_st = (sco_stats_t){0};
    taskEXIT_CRITICAL(&s_sco_mux);
    int64_t el = now - s_sum_since;
    s_sum_since = now;
    uint32_t decs = s.plc;
    for (int i = 0; i < DEC_N; i++) decs += s.dec[i];
    if (s.pkts == 0 && decs == 0) return;
    static char line[400];
    PACED_LINE(line, "@sco %lld %lld.%01llds pkt=%lu ok=%lu err=%lu none=%lu part=%lu len60=%lu len30=%lu lenx=%lu h2=%lu "
           "h2mid=%lu snskip=%lu gapmax=%lldms | dec ok=%lu nosync=%lu hdr=%lu body=%lu audio=%lu crc=%lu inval=%lu "
           "other=%lu plc=%lu h2mid=%lu\n",
           (long long)(now / 1000000), (long long)(el / 1000000), (long long)(el / 100000 % 10), ul(s.pkts),
           ul(s.st[0]), ul(s.st[1]), ul(s.st[2]), ul(s.st[3]), ul(s.len60), ul(s.len30), ul(s.lenx), ul(s.h2_head),
           ul(s.h2_mid), ul(s.sn_skip), (long long)(s.gap_max_us / 1000), ul(s.dec[DEC_OK]), ul(s.dec[DEC_NOSYNC]),
           ul(s.dec[DEC_HDR]), ul(s.dec[DEC_BODY]), ul(s.dec[DEC_AUDIO]), ul(s.dec[DEC_CRC]), ul(s.dec[DEC_INVAL]),
           ul(s.dec[DEC_OTHER]), ul(s.plc), ul(s.dec_h2_mid));
}

// ふだんどおりのもの（正しく届き、先頭に H2 ヘッダーがあるパケットと、その復号）
static bool sco_is_normal(const sco_ev_t* e) {
    if (e->kind == 'P') return e->a == 0 && e->h2 == 0;
    return !e->b[1] && e->a == DEC_OK && e->h2 == 0;
}

static const char* sec_us(char* buf, size_t n, int64_t t) {
    snprintf(buf, n, "%lld.%06lld", (long long)(t / 1000000), (long long)(t % 1000000));
    return buf;
}

// 頼まれた時刻の前後を 1 件 1 行で書く。ふだんどおりのものが続くところは、まとめて 1 行にする。
// 「@scod begin <理由> <頼まれた時刻> <件数>」〜「@scod end」
// 「@scod <起動からの秒> P <届いた印> <長さ> <H2 の位置> <先頭の 4 バイト>」
// 「@scod <起動からの秒> D <結果> <渡された長さ> <H2 の位置> <読んだ長さ>」（欠損補間のための復号は、結果を plc とする）
// 「@scod <起動からの秒> normal <件数> <最後の秒>」
static void sco_dump_run(int64_t now) {
    int64_t at = s_dump_at;
    if (at <= 0 || now < at + SCO_DUMP_AFTER_US) return;
    int64_t from = at - SCO_DUMP_BEFORE_US, to = at + SCO_DUMP_AFTER_US;
    // 書き出しはゆっくりなので、先に窓の分を写しておく
    uint32_t n = 0, head;
    taskENTER_CRITICAL(&s_sco_mux);
    head = s_sco_head;
    taskEXIT_CRITICAL(&s_sco_mux);
    for (uint32_t i = head > SCO_EVS ? head - SCO_EVS : 0; i < head; i++) {
        sco_ev_t e;
        taskENTER_CRITICAL(&s_sco_mux);
        bool gone = s_sco_head - i > SCO_EVS;
        e = s_sco_evs[i % SCO_EVS];
        taskEXIT_CRITICAL(&s_sco_mux);
        if (gone || e.t_us < from) continue;
        if (e.t_us > to) break;
        s_sco_snap[n++] = e;
    }

    static char line[96];
    char t1[24], t2[24];
    PACED_LINE(line, "@scod begin %s %s %lu\n", s_dump_why, sec_us(t1, sizeof(t1), at), ul(n));
    uint32_t run = 0;
    int64_t run_from = 0, run_to = 0;
    for (uint32_t k = 0; k <= n; k++) {
        const sco_ev_t* e = k < n ? &s_sco_snap[k] : NULL;
        if (e && sco_is_normal(e)) {
            if (!run) run_from = e->t_us;
            run_to = e->t_us;
            run++;
            continue;
        }
        if (run) {
            PACED_LINE(line, "@scod %s normal %lu %s\n", sec_us(t1, sizeof(t1), run_from), ul(run),
                       sec_us(t2, sizeof(t2), run_to));
            run = 0;
        }
        if (!e) break;
        char h2[4] = "-";
        if (e->h2 != H2_NONE) snprintf(h2, sizeof(h2), "%u", (unsigned)e->h2);
        if (e->kind == 'P') {
            PACED_LINE(line, "@scod %s P %u %u %s %02x%02x%02x%02x\n", sec_us(t1, sizeof(t1), e->t_us),
                       (unsigned)e->a, (unsigned)e->len, h2, e->b[0], e->b[1], e->b[2], e->b[3]);
        } else {
            PACED_LINE(line, "@scod %s D %s %u %s %u\n", sec_us(t1, sizeof(t1), e->t_us),
                       e->b[1] ? "plc" : kDecNames[e->a], (unsigned)e->len, h2, (unsigned)e->b[0]);
        }
    }
    PACED_LINE(line, "@scod end\n");
    s_dump_last = at;
    s_dump_at = 0;
}

// 1 件を「@hci <起動からの秒> <向き> <元の長さ> <16 進>」の 1 行で書く
static void hci_print_task(void* arg) {
    static char line[48 + HCI_SLOT_BYTES * 2];
    hci_slot_t s;
    for (;;) {
        if (s_sco_on) {
            int64_t now = esp_timer_get_time();
            sco_print_summary(now);
            sco_dump_run(now);
        }
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
        paced_write(line, n, sizeof(line));
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

void bt_diag_sco_log(bool on) {
    if (on) {
        if (!ensure_started()) return;
        if (!s_sco_evs) {
            s_sco_evs = heap_caps_malloc(sizeof(sco_ev_t) * SCO_EVS, MALLOC_CAP_SPIRAM);
            s_sco_snap = heap_caps_malloc(sizeof(sco_ev_t) * SCO_EVS, MALLOC_CAP_SPIRAM);
            if (!s_sco_evs || !s_sco_snap) {
                heap_caps_free(s_sco_evs);
                heap_caps_free(s_sco_snap);
                s_sco_evs = s_sco_snap = NULL;
                ESP_LOGE(TAG, "音声のパケットの記録の置き場を取れない");
                return;
            }
        }
        taskENTER_CRITICAL(&s_sco_mux);
        s_sco_st = (sco_stats_t){0};
        s_sco_head = 0;
        s_sco_handle = 0xFFFF;
        taskEXIT_CRITICAL(&s_sco_mux);
        s_sum_since = esp_timer_get_time();
    }
    s_sco_on = on;
}

bool bt_diag_sco_logging(void) { return s_sco_on; }

void bt_diag_sco_dump(const char* why) {
    if (!s_sco_on) return;
    int64_t now = esp_timer_get_time();
    bool take = false;
    taskENTER_CRITICAL(&s_sco_mux);
    if (s_dump_at == 0 && (s_dump_last == 0 || now - s_dump_last >= SCO_DUMP_MIN_GAP_US)) {
        s_dump_at = -1;  // 取った（理由を書き終えるまで、書き出しのタスクは待つ）
        take = true;
    }
    taskEXIT_CRITICAL(&s_sco_mux);
    if (!take) return;
    snprintf(s_dump_why, sizeof(s_dump_why), "%s", why);
    s_dump_at = now;
}
