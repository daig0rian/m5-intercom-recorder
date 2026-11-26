#include "bt_ag.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "app_state.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_ag_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "pcm_ring.h"
#include "settings.h"

static const char* TAG = "bt_ag";

extern "C" int bt_diag_role(const uint8_t* bda);  // bt_diag.c

namespace bt_ag {
namespace {

// 他社の商標を含めない名前にする
constexpr const char* kLocalName = "M5 Intercom Rec";
constexpr uint8_t kInquiryLen = 10;            // × 1.28 秒
constexpr int kMaxFound = 16;
constexpr uint64_t kAutoAudioDelayUs = 800 * 1000;  // SLC がつながってから音声を開くまで
constexpr uint64_t kTxTickUs = 5000;
constexpr int kPktStatEveryTicks = 400;        // 2 秒ごとにパケット統計を取る

portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
FoundDevice s_found[kMaxFound];
int s_found_count = 0;
int s_name_query = -1;  // 名前を問い合わせ中の機器の番号

uint8_t s_peer[6] = {};
bool s_has_peer = false;
// いまつなぎに行っている機器（アドレスと名前の組）。名前は、このアドレスの機器にだけ使う
uint8_t s_pending_bda[6] = {};
char s_pending_name[40] = "";
bool s_pending_valid = false;
bool s_connecting_saved = false;  // 保存済みの接続先に接続しようとしている
bool s_peer_bonded = false;       // 最後に接続した機器の鍵を Core2 が持っている

// 録音中に思いがけず切れたら、最後に接続した機器へ定期的につなぎ直しを試みる。B+COM は、離れて切れたあと
// 近づいても自分からはつなぎ直さない。録音中は接続先が変わらないので、つなぎ先を迷わない
constexpr uint64_t kReconnectIntervalUs = 10 * 1000 * 1000;
esp_timer_handle_t s_reconnect_timer = nullptr;
std::atomic<bool> s_reconnecting{false};
std::atomic<bool> s_disc_requested{false};  // こちらから切った（このときはつなぎ直さない）
int s_reconnect_tries = 0;

std::atomic<bool> s_audio_on{false};
std::atomic<uint32_t> s_rate{0};
uint16_t s_sync_handle = 0;

esp_timer_handle_t s_auto_audio_timer = nullptr;
esp_timer_handle_t s_tx_timer = nullptr;
std::atomic<int32_t> s_tx_credit{0};  // 送ってよいバイト数
int64_t s_tx_last_us = 0;
int s_tx_ticks = 0;
// 送信の合図（esp_hf_ag_outgoing_data_ready）を出してよいか。閉じる・切断を頼んだ時点で止める
std::atomic<bool> s_tx_active{false};
// スタックが送信データを取りに来たか。来なくなったら音声接続はもう閉じている
std::atomic<bool> s_tx_pulled{false};
int s_tx_idle_ticks = 0;
constexpr int kTxIdleLimit = 10;    // 50ms 取りに来なければ合図をやめる
constexpr int kTxProbeTicks = 200;  // やめている間も 1 秒に 1 回だけ様子を見る

// 音声接続の開閉を頼んだ時刻（0 = 頼んでいない）。完了の知らせが来るまでは次の開閉を受け付けない
std::atomic<uint32_t> s_audio_req_ms{0};
constexpr uint32_t kAudioReqTimeoutMs = 3000;
void mark_audio_request() { s_audio_req_ms.store(millis32() | 1); }

// 音声接続を開いておきたいか。メニューで閉じたら false にし、開くまで自動では開かない・開き直さない
std::atomic<bool> s_audio_wanted{true};

// 音声接続の開き直し（audio_reopen）。閉じたら開く。開けなければ、開けるか SLC が切れるまで試し続ける
std::atomic<bool> s_reopening{false};
esp_timer_handle_t s_reopen_timer = nullptr;
constexpr uint64_t kReopenOpenDelayUs = 500 * 1000;       // 閉じてから開くまで
constexpr uint64_t kReopenRetryUs = 5 * 1000 * 1000;      // 開けないときに試し直す間隔

// 受信パケットの誤り（2 秒ごとの割合）が多いことをログに残す。多くなったときと、減ったときに 1 行ずつ
constexpr int kErrHighPct = 25;
constexpr int kErrLowPct = 10;
bool s_err_high = false;
int64_t s_err_high_since_us = 0;
int s_err_high_max = 0;

// ループバック（受信した声を遅らせてインカムへ返す）
constexpr size_t kLoopbackCapacity = 16000 * 2;  // 16kHz で 2 秒分（遅れの分＋余裕）
PcmRing s_loop_ring;
std::atomic<bool> s_loop_reset{true};  // 次の送信でたまった分を捨てる
bool s_loop_playing = false;           // 送信コールバックの中だけで使う

void bda_to_str(const uint8_t* bda, char* out, size_t len) {
    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X", bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

// Core2 側にペアリング情報（リンクキー）が保存されているか
bool is_bonded(const uint8_t* bda) {
    esp_bd_addr_t list[8];
    int n = 8;
    if (esp_bt_gap_get_bond_device_list(&n, list) != ESP_OK) return false;
    for (int i = 0; i < n; i++) {
        if (memcmp(list[i], bda, 6) == 0) return true;
    }
    return false;
}

bool same_bda(const uint8_t* a, const uint8_t* b) { return memcmp(a, b, 6) == 0; }

void set_pending(const uint8_t* bda, const char* name) {
    memcpy(s_pending_bda, bda, 6);
    strlcpy(s_pending_name, name ? name : "", sizeof(s_pending_name));
    s_pending_valid = true;
}

void clear_pending() {
    s_pending_valid = false;
    s_pending_name[0] = 0;
}

// その機器の名前を、そのアドレスに対応する情報からだけ探す（別の機器の名前を当てはめない）
void name_for(const uint8_t* bda, char* out, size_t len) {
    if (s_pending_valid && same_bda(bda, s_pending_bda) && s_pending_name[0]) {
        strlcpy(out, s_pending_name, len);
        return;
    }
    if (settings::get_device_name(bda, out, len)) return;
    bool found = false;
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < s_found_count; i++) {
        if (same_bda(s_found[i].bda, bda) && s_found[i].name[0]) {
            strlcpy(out, s_found[i].name, len);
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    if (!found) bda_to_str(bda, out, len);
}

// つなぎに行って失敗したら、最後に接続した機器を保存してある値に戻す
void restore_saved_peer() {
    char name[40] = "";
    s_has_peer = settings::get_peer(s_peer, name, sizeof(name));
    if (!s_has_peer) memset(s_peer, 0, sizeof(s_peer));
    state_set_peer_name(s_has_peer ? name : "");
    s_peer_bonded = s_has_peer && is_bonded(s_peer);
}

// 最後に接続した機器を忘れる（「BT接続」は「機器検索」に戻る）
void forget_peer() {
    settings::clear_peer();
    s_has_peer = false;
    s_peer_bonded = false;
    memset(s_peer, 0, sizeof(s_peer));
    state_set_peer_name("");
}

// ---- 録音中のつなぎ直し -------------------------------------------------------

void start_reconnect() {
    if (s_reconnecting.exchange(true)) return;
    s_reconnect_tries = 0;
    char name[40];
    state_get_peer_name(name, sizeof(name));
    event_log("録音中に切れたので、%.30s につなぎ直します", name);
    esp_timer_start_periodic(s_reconnect_timer, kReconnectIntervalUs);
}

void stop_reconnect(const char* why) {
    if (!s_reconnecting.exchange(false)) return;
    esp_timer_stop(s_reconnect_timer);
    if (why) event_log("つなぎ直しをやめました（%s）", why);
}

// 試すたびにはログに書かない（つながらない間、ログがあふれるため）。失敗の知らせも、つなぎ直しの間は書かない
void reconnect_cb(void*) {
    if (!g_state.rec_requested.load()) return stop_reconnect("録音を止めた");
    if (!s_has_peer || !s_peer_bonded) return stop_reconnect("ペアリング情報がない");
    if (g_state.bt_state.load() != BtState::Idle) return;  // つなぎに行っている・向こうからつないできた
    s_reconnect_tries++;
    ESP_LOGI(TAG, "reconnect try %d", s_reconnect_tries);
    g_state.bt_state.store(BtState::Connecting);
    s_connecting_saved = true;
    esp_hf_ag_slc_connect(s_peer);
}

// ---- 音声データ ----------------------------------------------------------

// 受信（ヘッドセットのマイク音声）。CVSD は 8kHz、mSBC は復号済みの 16kHz の PCM で届く
void incoming_cb(const uint8_t* buf, uint32_t len) {
    g_state.rx_bytes.fetch_add(len, std::memory_order_relaxed);
    g_pcm_ring.write(buf, len / 2);
    if (g_state.loopback.load(std::memory_order_relaxed)) s_loop_ring.write(buf, len / 2);
}

// ループバック: 受信した声が遅れの分だけたまったら、送信に回す。途切れたら、またたまるまで無音
bool fill_loopback(uint8_t* buf, uint32_t len) {
    if (s_loop_reset.exchange(false)) {
        s_loop_ring.discard_all();
        s_loop_playing = false;
    }
    if (!g_state.loopback.load()) return false;
    size_t n = len / 2;
    if (!s_loop_playing) {
        if (s_loop_ring.available() < s_rate.load() * kLoopbackDelayMs / 1000) return false;
        s_loop_playing = true;
    }
    size_t got = s_loop_ring.read(buf, n);
    if (got < n) {
        memset(buf + got * 2, 0, (n - got) * 2);
        s_loop_playing = false;
    }
    return true;
}

// 送信。スタックは「データを返す限り」呼び続けるので、時間に見合った分だけ返す（ふだんは無音）
uint32_t outgoing_cb(uint8_t* buf, uint32_t len) {
    s_tx_pulled.store(true);
    int32_t credit = s_tx_credit.load();
    if (credit < static_cast<int32_t>(len)) return 0;
    s_tx_credit.fetch_sub(len);
    if (!fill_loopback(buf, len)) memset(buf, 0, len);
    return len;
}

// 音声接続が閉じたあとに合図を出し続けると、スタックが 1 回ごとにエラーを書き出す。
// その書き出し（約 5ms）が合図の間隔より長いため処理がたまり続け、Bluetooth 全体が止まる。
// そこで、スタックが送信データを取りに来なくなったら合図をやめる。
void tx_timer_cb(void*) {
    if (!s_audio_on.load() || !s_tx_active.load()) return;
    int64_t now = esp_timer_get_time();
    int64_t elapsed = now - s_tx_last_us;
    s_tx_last_us = now;
    int32_t bytes_per_sec = static_cast<int32_t>(s_rate.load()) * 2;
    int32_t add = static_cast<int32_t>(bytes_per_sec * elapsed / 1000000);
    int32_t cap = bytes_per_sec * 30 / 1000;  // 溜めるのは 30ms 分まで
    int32_t credit = std::min(s_tx_credit.load() + add, cap);
    s_tx_credit.store(credit);

    if (s_tx_pulled.exchange(false)) {
        if (s_tx_idle_ticks >= kTxIdleLimit) ESP_LOGI(TAG, "送信の取り込みが戻った");
        s_tx_idle_ticks = 0;
    } else if (credit >= cap) {
        if (++s_tx_idle_ticks == kTxIdleLimit) ESP_LOGW(TAG, "送信データを取りに来ないので合図を止める");
    }
    bool idle = s_tx_idle_ticks >= kTxIdleLimit;
    bool probe = idle && (s_tx_idle_ticks - kTxIdleLimit) % kTxProbeTicks == 0;
    if ((!idle || probe) && credit >= bytes_per_sec * 75 / 10000) {
        esp_hf_ag_outgoing_data_ready();  // 7.5ms 分たまったら
    }

    if (++s_tx_ticks >= kPktStatEveryTicks) {
        s_tx_ticks = 0;
        if (!idle) esp_hf_ag_pkt_stat_nums_get(s_sync_handle);
    }
}

// ---- 受信の欠け ------------------------------------------------------------
// パケット統計は音声接続を開いてからの累計で 2 秒ごとに届くので、前回との差から直近 2 秒の割合を出す

uint32_t s_drop_prev_total = 0, s_drop_prev_bad = 0;

void reset_drop_rate() {
    s_drop_prev_total = s_drop_prev_bad = 0;
    g_state.pkt_drop_x10.store(-1);
    s_err_high = false;
}

void update_drop_rate(uint32_t total, uint32_t bad) {
    // 数え直された（前回より小さい）ときは、今回の値をそのまま差分とみなす
    uint32_t d_total = total >= s_drop_prev_total ? total - s_drop_prev_total : total;
    uint32_t d_bad = bad >= s_drop_prev_bad ? bad - s_drop_prev_bad : bad;
    s_drop_prev_total = total;
    s_drop_prev_bad = bad;
    if (d_total == 0) return;
    int x10 = static_cast<int>(d_bad * 1000 / d_total);
    g_state.pkt_drop_x10.store(static_cast<int16_t>(x10));

    int pct = x10 / 10;
    if (!s_err_high && pct >= kErrHighPct) {
        s_err_high = true;
        s_err_high_since_us = esp_timer_get_time();
        s_err_high_max = pct;
        event_log("受信の誤りが多くなりました（%d%%）", pct);
    } else if (s_err_high) {
        s_err_high_max = std::max(s_err_high_max, pct);
        if (pct < kErrLowPct) {
            s_err_high = false;
            event_log("受信の誤りが減りました（多かったのは %lu 秒、最大 %d%%）",
                      (unsigned long)((esp_timer_get_time() - s_err_high_since_us) / 1000000), s_err_high_max);
        }
    }
}

// SLC がつながって少ししてから: 保存したマイク感度を指示し（インカムが対応している場合だけ）、音声接続を開く
void auto_audio_cb(void*) {
    if (g_state.bt_state.load() < BtState::Connected) return;
    int saved = settings::get_mic_gain();
    if (saved >= 0 && g_state.hf_remote_volume.load()) {
        esp_hf_ag_volume_control(s_peer, ESP_HF_VOLUME_CONTROL_TARGET_MIC, saved);
        g_state.hf_mic_gain.store(saved);
        event_log("マイク感度を %d に設定", saved);
    }
    if (!s_audio_on.load() && g_state.bt_state.load() == BtState::Connected) {
        if (!s_audio_wanted.load()) {
            event_log("音声接続はオフのままにします");
            return;
        }
        event_log("音声接続を開きます");
        mark_audio_request();
        esp_hf_ag_audio_connect(s_peer);
    }
}

// 開き直しの途中で、開けなかった・閉じなかったときに試し直す（開けるか SLC が切れるまで）
void reopen_cb(void*) {
    if (!s_reopening.load()) return;
    if (g_state.bt_state.load() < BtState::Connected) {
        s_reopening.store(false);
        esp_timer_stop(s_reopen_timer);
        return;
    }
    if (audio_busy()) return;
    mark_audio_request();
    if (s_audio_on.load()) {
        ESP_LOGW(TAG, "reopen: 閉じなかったので、もう一度閉じる");
        s_tx_active.store(false);
        esp_hf_ag_audio_disconnect(s_peer);
    } else {
        ESP_LOGW(TAG, "reopen: 開けなかったので、もう一度開く");
        esp_hf_ag_audio_connect(s_peer);
    }
}

void on_audio_connected(uint32_t rate, Codec codec, uint16_t handle) {
    if (s_reopening.exchange(false)) esp_timer_stop(s_reopen_timer);
    s_audio_req_ms.store(0);
    s_rate.store(rate);
    s_sync_handle = handle;
    g_state.codec.store(codec);
    reset_drop_rate();
    g_state.audio_sessions.fetch_add(1);
    g_state.bt_state.store(BtState::AudioOn);
    s_audio_on.store(true);

    esp_hf_ag_register_data_callback(incoming_cb, outgoing_cb);
    s_loop_reset.store(true);
    s_tx_credit.store(0);
    s_tx_pulled.store(false);
    s_tx_idle_ticks = 0;
    s_tx_active.store(true);
    s_tx_last_us = esp_timer_get_time();
    s_tx_ticks = 0;
    esp_timer_start_periodic(s_tx_timer, kTxTickUs);

    // 音声品質を保つため、接続中は他の機器から見えないようにする（サンプルと同じ）
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    pcm_source_changed(rate);
}

void on_audio_disconnected() {
    s_audio_req_ms.store(0);
    g_state.pkt_drop_x10.store(-1);
    s_tx_active.store(false);
    bool was_on = s_audio_on.exchange(false);
    esp_timer_stop(s_tx_timer);
    if (g_state.loopback.exchange(false)) event_log("ループバック終了（音声切断）");
    s_rate.store(0);
    g_state.codec.store(Codec::None);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    if (was_on) pcm_source_changed(0);
}

// ---- HFP AG イベント -----------------------------------------------------

void hf_cb(esp_hf_cb_event_t event, esp_hf_cb_param_t* p) {
    switch (event) {
        case ESP_HF_CONNECTION_STATE_EVT: {
            auto st = p->conn_stat.state;
            ESP_LOGI(TAG, "connection state %d, peer feat 0x%lx", st, (unsigned long)p->conn_stat.peer_feat);
            if (st == ESP_HF_CONNECTION_STATE_CONNECTING) {
                g_state.bt_state.store(BtState::Connecting);
            } else if (st == ESP_HF_CONNECTION_STATE_CONNECTED) {
                event_log("RFCOMM 接続");
            } else if (st == ESP_HF_CONNECTION_STATE_SLC_CONNECTED) {
                memcpy(s_peer, p->conn_stat.remote_bda, 6);
                s_has_peer = true;
                s_connecting_saved = false;
                s_disc_requested.store(false);
                if (s_reconnecting.load()) {
                    event_log("つなぎ直しました（%d 回目）", s_reconnect_tries);
                    stop_reconnect(nullptr);
                }
                g_state.bt_state.store(BtState::Connected);
                // HF の機能フラグ 0x10 = 音量・マイク感度の遠隔設定に対応（BTA_AG_PEER_FEAT_VOL）
                g_state.hf_remote_volume.store((p->conn_stat.peer_feat & 0x10) != 0);
                event_log("SLC 接続 (HF feat 0x%lx)", (unsigned long)p->conn_stat.peer_feat);
                {
                    // 実際につながった機器を「最後に接続した機器」にする（B2 で向こうからつないできた場合も）
                    char name[40];
                    name_for(s_peer, name, sizeof(name));
                    settings::set_peer(s_peer, name);
                    state_set_peer_name(name);
                    s_peer_bonded = is_bonded(s_peer);
                    clear_pending();
                    // 本人に名前を問い合わせ、控えを正しい名前にする（答えは READ_REMOTE_NAME で受ける）
                    if (s_name_query < 0) esp_bt_gap_read_remote_name(s_peer);
                }
                esp_timer_stop(s_auto_audio_timer);
                esp_timer_start_once(s_auto_audio_timer, kAutoAudioDelayUs);
            } else if (st == ESP_HF_CONNECTION_STATE_DISCONNECTED) {
                bool was_connected = g_state.bt_state.load() >= BtState::Connected;
                if (was_connected || !s_reconnecting.load()) event_log(was_connected ? "切断されました" : "接続できませんでした");
                if (!was_connected) {
                    // つなぎに行って失敗した: 選んでいた機器のことは忘れ、最後に接続した機器を元に戻す
                    clear_pending();
                    restore_saved_peer();
                }
                g_state.hf_mic_gain.store(-1);
                g_state.hf_remote_volume.store(false);
                s_connecting_saved = false;
                if (s_reopening.exchange(false)) esp_timer_stop(s_reopen_timer);
                on_audio_disconnected();
                g_state.bt_state.store(BtState::Idle);
                if (was_connected && !s_disc_requested.load() && g_state.rec_requested.load() && s_has_peer &&
                    s_peer_bonded) {
                    start_reconnect();
                }
            }
            break;
        }
        case ESP_HF_AUDIO_STATE_EVT: {
            auto st = p->audio_stat.state;
            if (st == ESP_HF_AUDIO_STATE_CONNECTED) {
                event_log("音声接続 CVSD 8kHz");
                on_audio_connected(8000, Codec::Cvsd, p->audio_stat.sync_conn_handle);
            } else if (st == ESP_HF_AUDIO_STATE_CONNECTED_MSBC) {
                event_log("音声接続 mSBC 16kHz");
                on_audio_connected(16000, Codec::Msbc, p->audio_stat.sync_conn_handle);
            } else if (st == ESP_HF_AUDIO_STATE_DISCONNECTED) {
                event_log("音声切断");
                on_audio_disconnected();
                if (g_state.bt_state.load() == BtState::AudioOn) g_state.bt_state.store(BtState::Connected);
                // 開き直しの途中なら、少し置いてから開く（auto_audio_cb が開く）
                if (s_reopening.load() && g_state.bt_state.load() == BtState::Connected) {
                    esp_timer_stop(s_auto_audio_timer);
                    esp_timer_start_once(s_auto_audio_timer, kReopenOpenDelayUs);
                }
            }
            break;
        }
        case ESP_HF_BVRA_RESPONSE_EVT:
            // ヘッドセットのボタンで音声認識を求められた → 音声接続を開いて応える
            event_log("AT+BVRA=%d", p->vra_rep.value);
            if (p->vra_rep.value == ESP_HF_VR_STATE_ENABLED && !s_audio_on.load()) {
                s_audio_wanted.store(true);  // インカムのボタンで頼まれた
                esp_hf_ag_audio_connect(p->vra_rep.remote_addr);
            }
            break;
        case ESP_HF_VOLUME_CONTROL_EVT:
            // インカムが自分の音量・マイク感度を知らせてきた（AT+VGS / AT+VGM）
            event_log("インカムの%s %d", p->volume_control.type == ESP_HF_VOLUME_TYPE_SPK ? "音量" : "マイク感度",
                      p->volume_control.volume);
            if (p->volume_control.type == ESP_HF_VOLUME_TYPE_MIC) g_state.hf_mic_gain.store(p->volume_control.volume);
            break;
        case ESP_HF_UNAT_RESPONSE_EVT:
            event_log("不明なAT: %.30s", p->unat_rep.unat ? p->unat_rep.unat : "");
            esp_hf_ag_unknown_at_send(p->unat_rep.remote_addr, nullptr);
            break;
        case ESP_HF_IND_UPDATE_EVT:
            ESP_LOGI(TAG, "indicator update");
            break;
        case ESP_HF_CIND_RESPONSE_EVT: {
            // 通話なし・電波あり・電池満タン、として答える
            esp_hf_ag_cind_response(p->cind_rep.remote_addr, ESP_HF_CALL_STATUS_NO_CALLS, ESP_HF_CALL_SETUP_STATUS_IDLE,
                                    ESP_HF_NETWORK_STATE_AVAILABLE, 5, ESP_HF_ROAMING_STATUS_INACTIVE, 5,
                                    ESP_HF_CALL_HELD_STATUS_NONE);
            break;
        }
        case ESP_HF_COPS_RESPONSE_EVT:
            esp_hf_ag_cops_response(p->cops_rep.remote_addr, const_cast<char*>("REC"));
            break;
        case ESP_HF_CLCC_RESPONSE_EVT:
            // 通話一覧: 通話はないので OK だけ返す
            esp_hf_ag_clcc_response(p->clcc_rep.remote_addr, 0, ESP_HF_CURRENT_CALL_DIRECTION_OUTGOING,
                                    ESP_HF_CURRENT_CALL_STATUS_ACTIVE, ESP_HF_CURRENT_CALL_MODE_VOICE,
                                    ESP_HF_CURRENT_CALL_MPTY_TYPE_SINGLE, nullptr, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
            break;
        case ESP_HF_CNUM_RESPONSE_EVT:
            esp_hf_ag_cmee_send(p->cnum_rep.remote_addr, ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
            break;
        case ESP_HF_NREC_RESPONSE_EVT:
            event_log("NREC=%d", p->nrec.state);
            break;
        case ESP_HF_ATA_RESPONSE_EVT:
            event_log("ATA（応答）");
            esp_hf_ag_cmee_send(p->ata_rep.remote_addr, ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
            if (!s_audio_on.load()) esp_hf_ag_audio_connect(p->ata_rep.remote_addr);
            break;
        case ESP_HF_CHUP_RESPONSE_EVT:
            event_log("AT+CHUP（終話）");
            esp_hf_ag_cmee_send(p->chup_rep.remote_addr, ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
            break;
        case ESP_HF_DIAL_EVT:
            // ヘッドセットのボタンで発信（リダイヤルなど）を求められた → OK を返して音声を開く
            event_log("発信要求 type=%d %.16s", p->out_call.type, p->out_call.num_or_loc ? p->out_call.num_or_loc : "");
            esp_hf_ag_cmee_send(p->out_call.remote_addr, ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
            if (!s_audio_on.load()) esp_hf_ag_audio_connect(p->out_call.remote_addr);
            break;
        case ESP_HF_WBS_RESPONSE_EVT:
            event_log("WBS 対応=%d", p->wbs_rep.codec);
            break;
        case ESP_HF_BCS_RESPONSE_EVT:
            event_log("コーデック決定=%d", p->bcs_rep.mode);
            break;
        case ESP_HF_PKT_STAT_NUMS_GET_EVT: {
            auto& n = p->pkt_nums;
            update_drop_rate(n.rx_total, n.rx_err + n.rx_none + n.rx_lost);
            ESP_LOGI(TAG, "pkt rx total=%lu ok=%lu err=%lu none=%lu lost=%lu", (unsigned long)n.rx_total,
                     (unsigned long)n.rx_correct, (unsigned long)n.rx_err, (unsigned long)n.rx_none,
                     (unsigned long)n.rx_lost);
            break;
        }
        case ESP_HF_PROF_STATE_EVT:
            ESP_LOGI(TAG, "profile state %d", p->prof_stat.state);
            break;
        default:
            ESP_LOGI(TAG, "HF event %d", event);
            break;
    }
}

// ---- GAP（検索・ペアリング）--------------------------------------------

void add_found(const uint8_t* bda, const char* name, int8_t rssi, uint32_t cod) {
    taskENTER_CRITICAL(&s_lock);
    int idx = -1;
    for (int i = 0; i < s_found_count; i++) {
        if (memcmp(s_found[i].bda, bda, 6) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0 && s_found_count < kMaxFound) {
        idx = s_found_count++;
        memset(&s_found[idx], 0, sizeof(FoundDevice));
        memcpy(s_found[idx].bda, bda, 6);
    }
    if (idx >= 0) {
        FoundDevice& d = s_found[idx];
        if (name && *name) strlcpy(d.name, name, sizeof(d.name));
        if (rssi != 0) d.rssi = rssi;
        if (cod) {
            d.cod = cod;
            d.is_audio = esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_AV;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
}

// 名前が分からない機器に、順番に名前を問い合わせる
void query_next_name() {
    uint8_t bda[6];
    bool found = false;
    taskENTER_CRITICAL(&s_lock);
    for (int i = s_name_query + 1; i < s_found_count; i++) {
        if (s_found[i].name[0] == '\0') {
            s_name_query = i;
            memcpy(bda, s_found[i].bda, 6);
            found = true;
            break;
        }
    }
    if (!found) s_name_query = -1;
    taskEXIT_CRITICAL(&s_lock);

    if (found) {
        esp_bt_gap_read_remote_name(bda);
    } else if (g_state.bt_state.load() == BtState::Discovering) {
        g_state.bt_state.store(BtState::Idle);
    }
}

void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* p) {
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            char name[40] = "";
            int8_t rssi = 0;
            uint32_t cod = 0;
            for (int i = 0; i < p->disc_res.num_prop; i++) {
                auto& prop = p->disc_res.prop[i];
                switch (prop.type) {
                    case ESP_BT_GAP_DEV_PROP_BDNAME: {
                        int n = std::min<int>(prop.len, sizeof(name) - 1);
                        memcpy(name, prop.val, n);
                        name[n] = '\0';
                        break;
                    }
                    case ESP_BT_GAP_DEV_PROP_COD:
                        cod = *static_cast<uint32_t*>(prop.val);
                        break;
                    case ESP_BT_GAP_DEV_PROP_RSSI:
                        rssi = *static_cast<int8_t*>(prop.val);
                        break;
                    case ESP_BT_GAP_DEV_PROP_EIR:
                        if (name[0] == '\0') {
                            uint8_t len = 0;
                            uint8_t* eir = static_cast<uint8_t*>(prop.val);
                            uint8_t* n = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
                            if (!n) n = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
                            if (n) {
                                int m = std::min<int>(len, sizeof(name) - 1);
                                memcpy(name, n, m);
                                name[m] = '\0';
                            }
                        }
                        break;
                    default:
                        break;
                }
            }
            add_found(p->disc_res.bda, name, rssi, cod);
            break;
        }
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            if (p->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
                ESP_LOGI(TAG, "discovery stopped, %d devices", s_found_count);
                if (g_state.bt_state.load() == BtState::Discovering) {
                    s_name_query = -1;
                    query_next_name();
                }
            }
            break;
        case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
            if (p->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
                add_found(p->read_rmt_name.bda, reinterpret_cast<const char*>(p->read_rmt_name.rmt_name), 0, 0);
                const char* nm = reinterpret_cast<const char*>(p->read_rmt_name.rmt_name);
                // 本人から聞いた名前で控えを直す
                if (nm[0] && is_bonded(p->read_rmt_name.bda)) settings::set_device_name(p->read_rmt_name.bda, nm);
                if (nm[0] && s_has_peer && same_bda(p->read_rmt_name.bda, s_peer)) {
                    state_set_peer_name(nm);
                    settings::set_peer(s_peer, nm);
                }
            }
            if (s_name_query >= 0) query_next_name();
            break;
        case ESP_BT_GAP_AUTH_CMPL_EVT: {
            bool is_peer = s_has_peer && memcmp(p->auth_cmpl.bda, s_peer, 6) == 0;
            if (p->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                event_log("認証OK %.20s (鍵種別 %d)", reinterpret_cast<const char*>(p->auth_cmpl.device_name),
                          p->auth_cmpl.lk_type);
                {
                    // 名前を控える（機器一覧で使う）。認証の知らせに名前がなければ、一覧で選んだときの名前
                    const char* dn = reinterpret_cast<const char*>(p->auth_cmpl.device_name);
                    if (dn[0]) {
                        settings::set_device_name(p->auth_cmpl.bda, dn);
                    } else if (s_pending_valid && same_bda(p->auth_cmpl.bda, s_pending_bda) && s_pending_name[0]) {
                        settings::set_device_name(p->auth_cmpl.bda, s_pending_name);  // つなぎに行った本人のときだけ
                    }
                }
                if (is_peer) s_peer_bonded = true;
                break;
            }
            event_log("認証失敗 (%d)", p->auth_cmpl.stat);
            // 鍵を持っていたのに認証に失敗し、スタックが鍵を消した = 相手側に鍵が無い（相手に忘れられた）。
            // 相手が応答しなかっただけのとき（電源が切れているなど）は鍵が消えないので、忘れない
            if (is_peer && s_peer_bonded && !is_bonded(s_peer)) {
                char name[40];
                state_get_peer_name(name, sizeof(name));
                event_log("%s 側にペアリングキーが無いようです。", name);
                event_log("もう一度ペアリングしてください。");
                forget_peer();
            }
            break;
        }
        case ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT:
            ESP_LOGI(TAG, "bond removed, stat %d", p->remove_bond_dev_cmpl.status);
            break;
        case ESP_BT_GAP_PIN_REQ_EVT: {
            // 古い方式のペアリング。インカムは 0000 が一般的
            esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
            esp_bt_gap_pin_reply(p->pin_req.bda, true, 4, pin);
            event_log("PIN 0000 で応答");
            break;
        }
        case ESP_BT_GAP_CFM_REQ_EVT:
            event_log("新規ペアリング（確認番号 %06lu）", (unsigned long)p->cfm_req.num_val);
            esp_bt_gap_ssp_confirm_reply(p->cfm_req.bda, true);
            break;
        case ESP_BT_GAP_KEY_NOTIF_EVT:
            ESP_LOGI(TAG, "passkey %06lu", (unsigned long)p->key_notif.passkey);
            break;
        case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
            ESP_LOGI(TAG, "ACL connected, stat %d, Core2 は%s", p->acl_conn_cmpl_stat.stat,
                     bt_diag_role(p->acl_conn_cmpl_stat.bda) == 0 ? "親機" : "子機");
            break;
        case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
            ESP_LOGI(TAG, "ACL disconnected, reason 0x%x", p->acl_disconn_cmpl_stat.reason);
            break;
        default:
            break;
    }
}

}  // namespace

void init() {
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bd_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    char addr[18];
    bda_to_str(esp_bt_dev_get_address(), addr, sizeof(addr));
    ESP_LOGI(TAG, "own address %s", addr);

    s_loop_ring.init(kLoopbackCapacity);

    esp_timer_create_args_t a = {};
    a.callback = auto_audio_cb;
    a.name = "auto_audio";
    ESP_ERROR_CHECK(esp_timer_create(&a, &s_auto_audio_timer));
    esp_timer_create_args_t t = {};
    t.callback = tx_timer_cb;
    t.name = "hf_tx";
    ESP_ERROR_CHECK(esp_timer_create(&t, &s_tx_timer));
    esp_timer_create_args_t r = {};
    r.callback = reconnect_cb;
    r.name = "reconnect";
    ESP_ERROR_CHECK(esp_timer_create(&r, &s_reconnect_timer));
    esp_timer_create_args_t o = {};
    o.callback = reopen_cb;
    o.name = "reopen";
    ESP_ERROR_CHECK(esp_timer_create(&o, &s_reopen_timer));

    esp_bt_gap_register_callback(gap_cb);
    esp_bt_gap_set_device_name(kLocalName);

    // 機器の種類: 電話（スマートフォン）。サービス: 電話・音声
    esp_bt_cod_t cod = {};
    cod.major = ESP_BT_COD_MAJOR_DEV_PHONE;
    cod.minor = 0x03;  // smartphone
    cod.service = ESP_BT_COD_SRVC_TELEPHONY | ESP_BT_COD_SRVC_AUDIO;
    esp_bt_gap_set_cod(cod, ESP_BT_INIT_COD);

    // 簡単ペアリング（画面・入力なし同士の Just Works）
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin);

    esp_hf_ag_register_callback(hf_cb);
    esp_hf_ag_init();

    // インカム側から接続しに来られるように（B2 を押されたとき）
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    // 最後に接続した機器と、その鍵は保存している係が別（接続先はこのプロジェクト、鍵は Bluedroid）。
    // 「接続先を覚えている ⇔ 鍵もある」を保つため、鍵が無ければ接続先も忘れる
    char name[40] = "";
    s_has_peer = settings::get_peer(s_peer, name, sizeof(name));
    if (s_has_peer) {
        if (!name[0]) bda_to_str(s_peer, name, sizeof(name));
        state_set_peer_name(name);
        s_peer_bonded = is_bonded(s_peer);
        if (s_peer_bonded) {
            event_log("最後に接続した機器：%s", name);
            settings::set_device_name(s_peer, name);  // 名前の控えがない（前の版でペアリングした）ときのため
        } else {
            event_log("%s のペアリングキーが見つかりません。", name);
            event_log("もう一度ペアリングしてください。");
            forget_peer();
        }
    } else {
        event_log("最後に接続した機器：なし");
    }
    g_state.bt_state.store(BtState::Idle);
}

void start_discovery() {
    BtState st = g_state.bt_state.load();
    if (st != BtState::Idle && st != BtState::Discovering) return;
    taskENTER_CRITICAL(&s_lock);
    s_found_count = 0;
    s_name_query = -1;
    taskEXIT_CRITICAL(&s_lock);
    g_state.bt_state.store(BtState::Discovering);
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, kInquiryLen, 0);
}

void stop_discovery() {
    if (g_state.bt_state.load() != BtState::Discovering) return;
    s_name_query = -1;
    g_state.bt_state.store(BtState::Idle);
    esp_bt_gap_cancel_discovery();
}

int found_devices(FoundDevice* out, int max) {
    taskENTER_CRITICAL(&s_lock);
    int n = std::min(s_found_count, max);
    memcpy(out, s_found, n * sizeof(FoundDevice));
    taskEXIT_CRITICAL(&s_lock);
    // オーディオ機器を先に、その中は電波の強い順
    std::stable_sort(out, out + n, [](const FoundDevice& a, const FoundDevice& b) {
        if (a.is_audio != b.is_audio) return a.is_audio;
        return a.rssi > b.rssi;
    });
    return n;
}

bool is_paired(const uint8_t bda[6]) { return is_bonded(bda); }

int paired_devices(PairedDevice* out, int max) {
    esp_bd_addr_t list[16];
    int n = 16;
    if (esp_bt_gap_get_bond_device_list(&n, list) != ESP_OK) return 0;
    n = std::min(n, max);
    bool connected = g_state.bt_state.load() >= BtState::Connected;
    for (int i = 0; i < n; i++) {
        PairedDevice& d = out[i];
        memcpy(d.bda, list[i], 6);
        d.last = s_has_peer && memcmp(d.bda, s_peer, 6) == 0;
        d.connected = d.last && connected;
        if (!settings::get_device_name(d.bda, d.name, sizeof(d.name))) {
            if (d.last) state_get_peer_name(d.name, sizeof(d.name));
            else bda_to_str(d.bda, d.name, sizeof(d.name));
        }
    }
    // 最後に接続した機器を先に
    std::stable_sort(out, out + n, [](const PairedDevice& a, const PairedDevice& b) { return a.last && !b.last; });
    return n;
}

void connect_paired(const uint8_t bda[6], const char* name) {
    BtState st = g_state.bt_state.load();
    if (st >= BtState::Connecting) {
        if (s_has_peer && memcmp(bda, s_peer, 6) == 0) return;  // もうつながっている（つなぎに行っている）
        disconnect();  // 別の機器につながっていれば切る（つなぎ直しは切断のあとで）
        return;
    }
    stop_discovery();
    s_disc_requested.store(false);
    memcpy(s_peer, bda, 6);
    s_has_peer = true;
    s_peer_bonded = true;
    set_pending(bda, name);
    state_set_peer_name(s_pending_name);
    g_state.bt_state.store(BtState::Connecting);
    event_log("接続: %.30s", s_pending_name);
    esp_hf_ag_slc_connect(s_peer);
}

void remove_pairing(const uint8_t bda[6]) {
    char name[40];
    if (!settings::get_device_name(bda, name, sizeof(name))) bda_to_str(bda, name, sizeof(name));
    bool is_peer = s_has_peer && memcmp(bda, s_peer, 6) == 0;
    if (is_peer && g_state.bt_state.load() >= BtState::Connecting) disconnect();
    uint8_t copy[6];
    memcpy(copy, bda, 6);
    esp_bt_gap_remove_bond_device(copy);
    settings::remove_device_name(copy);
    if (is_peer) forget_peer();  // 鍵を消したら、最後に接続した機器としても忘れる
    event_log("ペアリングを削除: %.30s", name);
}

void connect_to(const uint8_t bda[6], const char* name) {
    stop_discovery();
    s_disc_requested.store(false);
    memcpy(s_peer, bda, 6);
    s_has_peer = true;
    set_pending(bda, name);
    s_connecting_saved = false;
    g_state.bt_state.store(BtState::Connecting);
    // 一覧から選び直したときは、古いペアリング情報を消してから新しくペアリングする
    if (is_bonded(s_peer)) {
        event_log("古いペアリング情報を消去");
        esp_bt_gap_remove_bond_device(s_peer);
    }
    s_peer_bonded = false;
    event_log("接続: %.30s", name && *name ? name : "(名前なし)");
    esp_hf_ag_slc_connect(s_peer);
}

bool connect_saved() {
    if (!s_has_peer) return false;
    BtState st = g_state.bt_state.load();
    if (st != BtState::Idle) return true;
    s_disc_requested.store(false);
    g_state.bt_state.store(BtState::Connecting);
    s_connecting_saved = true;
    char name[40];
    state_get_peer_name(name, sizeof(name));
    set_pending(s_peer, name);
    event_log("接続: %.30s%s", name, is_bonded(s_peer) ? "" : "（未ペアリング）");
    esp_hf_ag_slc_connect(s_peer);
    return true;
}

bool reconnecting() { return s_reconnecting.load(); }

int link_role() { return s_has_peer ? bt_diag_role(s_peer) : -1; }

void disconnect() {
    s_disc_requested.store(true);  // 自分で切ったときは、つなぎ直さない
    stop_reconnect("切断した");
    s_tx_active.store(false);  // 先に送信の合図を止める
    esp_timer_stop(s_auto_audio_timer);
    if (g_state.bt_state.load() >= BtState::Connecting) esp_hf_ag_slc_disconnect(s_peer);
}

bool audio_busy() {
    uint32_t t = s_audio_req_ms.load();
    return t != 0 && millis32() - t < kAudioReqTimeoutMs;
}

void audio_open() {
    s_audio_wanted.store(true);
    if (audio_busy() || g_state.bt_state.load() != BtState::Connected) return;
    mark_audio_request();
    event_log("音声接続を開きます");
    esp_hf_ag_audio_connect(s_peer);
}

bool audio_wanted() { return s_audio_wanted.load(); }

void audio_close() {
    s_audio_wanted.store(false);
    if (audio_busy() || !s_audio_on.load()) return;
    if (s_reopening.exchange(false)) esp_timer_stop(s_reopen_timer);  // 閉じたままにする
    mark_audio_request();
    s_tx_active.store(false);  // 先に送信の合図を止める
    event_log("音声接続を閉じます");
    esp_hf_ag_audio_disconnect(s_peer);
}

bool audio_reopen() {
    if (!s_audio_on.load() || !s_audio_wanted.load() || audio_busy() || s_reopening.load()) return false;
    s_reopening.store(true);
    event_log("音声接続を開き直します");
    mark_audio_request();
    s_tx_active.store(false);  // 先に送信の合図を止める
    esp_hf_ag_audio_disconnect(s_peer);
    esp_timer_stop(s_reopen_timer);
    esp_timer_start_periodic(s_reopen_timer, kReopenRetryUs);
    return true;
}

void set_mic_gain(int gain) {
    if (g_state.bt_state.load() < BtState::Connected || !g_state.hf_remote_volume.load()) return;
    gain = std::clamp(gain, 0, 15);
    esp_hf_ag_volume_control(s_peer, ESP_HF_VOLUME_CONTROL_TARGET_MIC, gain);
    g_state.hf_mic_gain.store(gain);
    settings::set_mic_gain(gain);
    event_log("マイク感度 %d", gain);
}

void set_loopback(bool on) {
    if (g_state.loopback.load() == on) return;
    s_loop_reset.store(true);
    g_state.loopback.store(on);
    if (on) event_log("ループバック開始（%.1f 秒遅れ）", kLoopbackDelayMs / 1000.0);
    else event_log("ループバック終了");
}

}  // namespace bt_ag
