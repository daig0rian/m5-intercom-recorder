// 調べるための仕掛け: Bluetooth の接続での Core2 の役割（親機 = Central、子機 = Peripheral）を知る。
// 役割が入れ替わったとき（HCI の Role Change）は、シリアルにログを出す。
//
// IDF は書き換えず、リンク時に差し替える（main/CMakeLists.txt の --wrap）。btm_acl_role_changed は
// btu_hcif.c から呼ばれる（同じファイルの中からの呼び出しは差し替わらない）。IDF を上げたら確かめる。

#include "common/bt_target.h"
#include "stack/btm_api.h"
#include "esp_log.h"

static const char* TAG = "bt_diag";

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

// 0 = 親機、1 = 子機、-1 = つながっていない
int bt_diag_role(const uint8_t* bda) {
    UINT8 role;
    if (BTM_GetRole((UINT8*)bda, &role) != BTM_SUCCESS) return -1;
    return role == BTM_ROLE_MASTER ? 0 : 1;
}
