#include "settings.h"

#include <cstring>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "settings";
static const char* kNamespace = "recorder";
static constexpr uint32_t kDefaultSplitSec = 30 * 60;

namespace settings {

void init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS を初期化し直します");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

bool get_peer(uint8_t bda[6], char* name, size_t name_len) {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 6;
    bool ok = nvs_get_blob(h, "peer_bda", bda, &len) == ESP_OK && len == 6;
    if (ok && name && name_len) {
        size_t nlen = name_len;
        if (nvs_get_str(h, "peer_name", name, &nlen) != ESP_OK) name[0] = '\0';
    }
    nvs_close(h);
    return ok;
}

void set_peer(const uint8_t bda[6], const char* name) {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "peer_bda", bda, 6);
    nvs_set_str(h, "peer_name", name ? name : "");
    nvs_commit(h);
    nvs_close(h);
}

namespace {
struct NamedDevice {
    uint8_t bda[6];
    char name[34];
};

void load_names(NamedDevice (&list)[kMaxNamedDevices]) {
    memset(list, 0, sizeof(list));
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(list);
    nvs_get_blob(h, "dev_names", list, &len);
    nvs_close(h);
}

void save_names(const NamedDevice (&list)[kMaxNamedDevices]) {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "dev_names", list, sizeof(list));
    nvs_commit(h);
    nvs_close(h);
}

bool is_empty(const NamedDevice& d) {
    static const uint8_t zero[6] = {};
    return memcmp(d.bda, zero, 6) == 0;
}
}  // namespace

void set_device_name(const uint8_t bda[6], const char* name) {
    if (!name || !*name) return;
    NamedDevice list[kMaxNamedDevices];
    load_names(list);
    int slot = -1;
    for (int i = 0; i < kMaxNamedDevices; i++) {
        if (memcmp(list[i].bda, bda, 6) == 0) {
            slot = i;
            break;
        }
        if (slot < 0 && is_empty(list[i])) slot = i;
    }
    if (slot < 0) slot = 0;  // いっぱいなら先頭（いちばん古い）を上書き
    memcpy(list[slot].bda, bda, 6);
    strlcpy(list[slot].name, name, sizeof(list[slot].name));
    save_names(list);
}

bool get_device_name(const uint8_t bda[6], char* name, size_t name_len) {
    NamedDevice list[kMaxNamedDevices];
    load_names(list);
    for (auto& d : list) {
        if (!is_empty(d) && memcmp(d.bda, bda, 6) == 0) {
            strlcpy(name, d.name, name_len);
            return true;
        }
    }
    return false;
}

void remove_device_name(const uint8_t bda[6]) {
    NamedDevice list[kMaxNamedDevices];
    load_names(list);
    for (auto& d : list) {
        if (memcmp(d.bda, bda, 6) == 0) memset(&d, 0, sizeof(d));
    }
    save_names(list);
}

void clear_peer() {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "peer_bda");
    nvs_erase_key(h, "peer_name");
    nvs_commit(h);
    nvs_close(h);
}

int get_mic_gain() {
    nvs_handle_t h;
    int8_t v = -1;
    if (nvs_open(kNamespace, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i8(h, "mic_gain", &v);
        nvs_close(h);
    }
    return v;
}

void set_mic_gain(int gain) {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i8(h, "mic_gain", static_cast<int8_t>(gain));
    nvs_commit(h);
    nvs_close(h);
}

uint32_t get_split_sec() {
    nvs_handle_t h;
    uint32_t v = kDefaultSplitSec;
    if (nvs_open(kNamespace, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "split_sec", &v);
        nvs_close(h);
    }
    return v;
}

void set_split_sec(uint32_t sec) {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "split_sec", sec);
    nvs_commit(h);
    nvs_close(h);
}

bool get_log_to_sd() {
    nvs_handle_t h;
    uint8_t v = 1;
    if (nvs_open(kNamespace, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "log_to_sd", &v);
        nvs_close(h);
    }
    return v != 0;
}

void set_log_to_sd(bool on) {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "log_to_sd", on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

bool get_reconnect_on_boot() {
    nvs_handle_t h;
    uint8_t v = 1;
    if (nvs_open(kNamespace, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "reconnect", &v);
        nvs_close(h);
    }
    return v != 0;
}

void set_reconnect_on_boot(bool on) {
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "reconnect", on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

bool erase_all() {
    esp_err_t err = nvs_flash_erase();  // 開いているハンドルは閉じてから消す
    if (err != ESP_OK) ESP_LOGE(TAG, "NVS の消去に失敗: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

}  // namespace settings
