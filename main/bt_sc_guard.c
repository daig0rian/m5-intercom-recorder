// Bluedroid の「SC からの格下げ」の判定を、Core2 が SC を使っていないときは効かせない。
//
// Bluedroid は、鍵ができたときに相手が名乗った「SC（Secure Connections）に対応」を保存する。
// 次の接続で相手が「非対応」と名乗ると、格下げの攻撃を疑って鍵を消し、接続を切る
// （btm_sec_set_peer_sec_caps）。このとき、自分が SC を使っているかも、鍵の種別も見ない。
// Core2 は SC を使わない（bt_ag.cpp の sc_en = false）ので、鍵はいつも古い方式（P-192）になる。
// そのためこの判定は何も守らず、接続ごとに名乗りが変わる機器（実際に市販のヘッドセットであった）の
// 鍵を消すだけになる。
//
// IDF は書き換えず、リンク時に差し替える（main/CMakeLists.txt の --wrap）。
// Core2 が SC を使うように変えたときは、元の判定のまま動く。
// Bluedroid の内部の構造体を使うので、IDF を上げたら、ビルドが通ることと、この関数がまだ
// btm_acl.c から呼ばれていること（同じファイルの中からの呼び出しは差し替わらない）を確かめる。

#include "common/bt_target.h"
#include "btm_int.h"
#include "device/controller.h"
#include "esp_log.h"

static const char* TAG = "bt_sc_guard";

void __real_btm_sec_set_peer_sec_caps(tACL_CONN* p_acl_cb, tBTM_SEC_DEV_REC* p_dev_rec);

void __wrap_btm_sec_set_peer_sec_caps(tACL_CONN* p_acl_cb, tBTM_SEC_DEV_REC* p_dev_rec) {
#if (CLASSIC_BT_INCLUDED == TRUE)
    if (p_dev_rec && !controller_get_interface()->supports_secure_connections()) {
        // 判定は「今回の名乗り ≧ 前回」なら通るので、前回を「非対応」にしてから判定させる
        BOOLEAN was_sc = p_dev_rec->remote_secure_connection_previous_state;
        p_dev_rec->remote_secure_connection_previous_state = FALSE;
        __real_btm_sec_set_peer_sec_caps(p_acl_cb, p_dev_rec);
        if (was_sc && !p_dev_rec->remote_supports_secure_connections) {
            ESP_LOGW(TAG, "相手が SC 非対応と名乗った（前回は対応）。Core2 は SC を使わないので鍵は残す");
        }
        return;
    }
#endif
    __real_btm_sec_set_peer_sec_caps(p_acl_cb, p_dev_rec);
}
