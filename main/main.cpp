// m5-intercom-recorder: バイク用インカムの通話音声を M5Stack Core2 で受け取り、SD カードに WAV で録音する
#include "M5Unified.h"
#include "app_state.h"
#include "audio_pipeline.h"
#include "bt_ag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "rtc_clock.h"
#include "settings.h"
#include "sound.h"
#include "spi_lock.h"
#include "storage.h"
#include "ui.h"

static const char* TAG = "main";

static void log_heap(const char* when) {
    ESP_LOGI(TAG, "[%s] 内部RAM 空き %u (最大ブロック %u) / PSRAM 空き %u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

extern "C" void app_main() {
    settings::init();  // NVS（M5GFX と Bluetooth も使う）
    g_state.split_ms.store(settings::get_split_sec() * 1000);
    g_state.log_to_sd.store(settings::get_log_to_sd());
    g_state.reconnect_on_boot.store(settings::get_reconnect_on_boot());
    event_log_init(storage::append_log);  // ログは画面用に PSRAM へ、SD にも書き足す
    spi_lock_init();

    auto cfg = M5.config();
    cfg.internal_mic = false;  // 内蔵マイクは使わない
    cfg.internal_spk = true;   // 操作音を鳴らす
    cfg.internal_imu = false;
    M5.begin(cfg);
    sound::init();
    M5.Display.setFont(&fonts::efontJA_16);
    M5.Display.drawString("起動中…", 10, 10);

    storage::mount();
    storage::start_writer_task();
    rtc_clock::init();  // 時計を合わせたログも SD に残るよう、書き込み係を先に動かす
    // 電池の残量の出し方は電源 IC で変わる（ui.cpp の read_power）ので、どちらだったかをログに残す
    const auto pmic = M5.Power.getType();
    event_log("電源 IC：%s", pmic == m5::Power_Class::pmic_axp192    ? "AXP192"
                            : pmic == m5::Power_Class::pmic_axp2101 ? "AXP2101"
                                                                     : "不明");
    audio_pipeline::start();
    log_heap("BT 前");

    bt_ag::init();
    log_heap("BT 後");

    ui::start();
    sound::play(sound::Cue::PowerOn);
    // 「起動時に再接続」がオンなら、「BT接続」ボタンと同じように最後に接続した機器へつなぎに行く（1 回だけ）
    if (g_state.reconnect_on_boot.load()) bt_ag::connect_saved();
    log_heap("起動完了");
}
