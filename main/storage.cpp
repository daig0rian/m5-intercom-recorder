#include "storage.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "app_state.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "spi_lock.h"

static const char* TAG = "storage";

namespace storage {
namespace {

// Core2 の SD カード（液晶と同じ SPI バス）
constexpr spi_host_device_t kSpiHost = SPI3_HOST;
constexpr gpio_num_t kPinSclk = GPIO_NUM_18;
constexpr gpio_num_t kPinMosi = GPIO_NUM_23;
constexpr gpio_num_t kPinMiso = GPIO_NUM_38;
constexpr gpio_num_t kPinCs = GPIO_NUM_4;
constexpr int kSdFreqKhz = 20000;

constexpr uint32_t kFsyncIntervalMs = 5000;
constexpr uint32_t kFreeSpaceIntervalMs = 30000;
// Core2 の SD スロットにはカード検出の信号がないので、カードに問い合わせて抜き差しを知る
constexpr uint32_t kCardCheckMs = 2000;  // 挿さっている間の確認の間隔
constexpr uint32_t kRemountMs = 3000;    // 抜かれている間に認識し直す間隔
constexpr int kQueueDepth = 64;  // Ogg ページ（約 1 秒分）× 64

enum class MsgType : uint8_t { Open, Data, Close, Log, FlushLog, Shutdown };

struct Msg {
    MsgType type;
    uint32_t len;
    uint8_t* data;
    char path[48];
    SemaphoreHandle_t done;  // Shutdown の完了を知らせる
};

QueueHandle_t s_queue = nullptr;
sdmmc_card_t* s_card = nullptr;
bool s_stopped = false;  // シャットダウン後は抜き差しの確認もしない

void update_free_space() {
    uint64_t total = 0, free_bytes = 0;
    esp_err_t err;
    {
        SpiLockGuard lock;
        err = esp_vfs_fat_info(kMountPoint, &total, &free_bytes);
    }
    if (err == ESP_OK) g_state.sd_free_mb.store(static_cast<uint32_t>(free_bytes / (1024 * 1024)));
}

bool enqueue(const Msg& m) {
    if (!s_queue || xQueueSend(s_queue, &m, 0) != pdTRUE) {
        g_state.write_queue_drops.fetch_add(1);
        return false;
    }
    return true;
}

// カードが入っていないときに毎回出るエラーログを抑える
void set_sd_log_level(esp_log_level_t level) {
    static const char* kTags[] = {"vfs_fat_sdmmc", "sdmmc_common", "sdmmc_sd",         "sdmmc_init",
                                  "sdspi_host",    "sdspi_transaction", "sdmmc_cmd", TAG};
    for (const char* t : kTags) esp_log_level_set(t, level);
}

bool try_mount(bool quiet) {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = kSpiHost;
    host.max_freq_khz = kSdFreqKhz;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = kPinCs;
    slot.host_id = kSpiHost;

    esp_err_t err;
    {
        SpiLockGuard lock;
        // 液晶側（M5GFX）がバスを初期化済みのはず。未初期化なら自分で初期化する
        // （初期化済みのときに出るエラーログは想定どおりなので抑える）
        esp_log_level_set("spi", ESP_LOG_NONE);
        spi_bus_config_t bus = {};
        bus.mosi_io_num = kPinMosi;
        bus.miso_io_num = kPinMiso;
        bus.sclk_io_num = kPinSclk;
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.max_transfer_sz = 4096;
        esp_err_t bus_err = spi_bus_initialize(kSpiHost, &bus, SPI_DMA_CH_AUTO);
        esp_log_level_set("spi", ESP_LOG_INFO);
        if (bus_err != ESP_OK && bus_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "spi_bus_initialize: %s", esp_err_to_name(bus_err));
        }
        if (quiet) set_sd_log_level(ESP_LOG_NONE);
        err = esp_vfs_fat_sdspi_mount(kMountPoint, &host, &slot, &mount_config, &s_card);
        if (quiet) set_sd_log_level(ESP_LOG_INFO);
    }
    if (err != ESP_OK) {
        if (!quiet) ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
        s_card = nullptr;
        g_state.sd_mounted.store(false);
        return false;
    }
    g_state.sd_mounted.store(true);
    if (!quiet) sdmmc_card_print_info(stdout, s_card);
    update_free_space();
    return true;
}

void writer_task(void*) {
    FILE* fp = nullptr;
    char* vbuf = static_cast<char*>(heap_caps_malloc(8192, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    uint32_t last_sync = millis32();
    uint32_t last_free = 0;
    uint32_t last_card = millis32();
    bool dirty = false;

    // ログのファイル（SD の LOG フォルダに日付ごと）。録音ファイルとは別に開いておく
    FILE* log_fp = nullptr;
    char log_date[12] = "";
    bool log_dirty = false;
    uint32_t last_log_sync = millis32();

    auto close_log = [&]() {
        if (!log_fp) return;
        SpiLockGuard lock;
        fflush(log_fp);
        fsync(fileno(log_fp));
        fclose(log_fp);
        log_fp = nullptr;
        log_dirty = false;
    };

    auto write_log = [&](const uint8_t* line, size_t len) {
        if (!g_state.sd_mounted.load()) return;
        time_t t = time(nullptr);
        struct tm tm;
        localtime_r(&t, &tm);
        char date[12];
        strftime(date, sizeof(date), "%Y%m%d", &tm);
        if (log_fp && strcmp(date, log_date) != 0) close_log();  // 日付が変わったら新しいファイル
        SpiLockGuard lock;
        if (!log_fp) {
            char path[40];
            snprintf(path, sizeof(path), "%s/%s.txt", kLogDir, date);
            mkdir(kLogDir, 0777);
            log_fp = fopen(path, "a");
            if (!log_fp) return;
            strlcpy(log_date, date, sizeof(log_date));
        }
        fwrite(line, 1, len, log_fp);
        fwrite("\n", 1, 1, log_fp);  // fputc は IRAM を 137 バイト余分に使う（__swbuf_r）ので fwrite にする
        log_dirty = true;
    };

    // カードが抜かれた: 開いているファイルを手放してアンマウントする
    auto on_removed = [&]() {
        SpiLockGuard lock;
        if (fp) fclose(fp);  // 書けなくても FILE は解放される
        fp = nullptr;
        dirty = false;
        if (log_fp) fclose(log_fp);
        log_fp = nullptr;
        log_dirty = false;
        esp_vfs_fat_sdcard_unmount(kMountPoint, s_card);
        s_card = nullptr;
        g_state.sd_mounted.store(false);
        g_state.sd_free_mb.store(0);
    };

    auto close_fp = [&]() {
        if (!fp) return;
        SpiLockGuard lock;
        fflush(fp);
        fsync(fileno(fp));
        fclose(fp);
        fp = nullptr;
        dirty = false;
    };

    for (;;) {
        Msg m;
        if (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(500)) == pdTRUE) {
            switch (m.type) {
                case MsgType::Open: {
                    close_fp();
                    SpiLockGuard lock;
                    mkdir(kRecDir, 0777);
                    fp = fopen(m.path, "wb");
                    if (fp) {
                        if (vbuf) setvbuf(fp, vbuf, _IOFBF, 8192);
                        ESP_LOGI(TAG, "open %s", m.path);
                    } else {
                        ESP_LOGE(TAG, "open %s failed: errno %d", m.path, errno);
                        g_state.write_errors.fetch_add(1);
                    }
                    last_sync = millis32();
                    break;
                }
                case MsgType::Data:
                    if (fp) {
                        size_t w;
                        {
                            SpiLockGuard lock;
                            w = fwrite(m.data, 1, m.len, fp);
                        }
                        if (w != m.len) {
                            g_state.write_errors.fetch_add(1);
                            last_card = 0;  // 抜かれたかもしれないので、すぐ確かめる
                        } else {
                            g_state.bytes_written.fetch_add(m.len);
                            dirty = true;
                        }
                    }
                    heap_caps_free(m.data);
                    break;
                case MsgType::Close:
                    close_fp();
                    ESP_LOGI(TAG, "closed");
                    break;
                case MsgType::Log:
                    write_log(m.data, m.len);
                    heap_caps_free(m.data);
                    break;
                case MsgType::FlushLog:
                    // ここまでに積まれたログは書き終えている。すぐ確定する
                    if (log_fp && log_dirty) {
                        SpiLockGuard lock;
                        fflush(log_fp);
                        fsync(fileno(log_fp));
                        log_dirty = false;
                        last_log_sync = millis32();
                    }
                    break;
                case MsgType::Shutdown:
                    // ここまでに積まれた書き込みは処理済み。ファイルを閉じて SD を外す
                    close_fp();
                    close_log();
                    if (g_state.sd_mounted.load()) {
                        SpiLockGuard lock;
                        esp_vfs_fat_sdcard_unmount(kMountPoint, s_card);
                        s_card = nullptr;
                        g_state.sd_mounted.store(false);
                    }
                    s_stopped = true;
                    ESP_LOGI(TAG, "shutdown");
                    if (m.done) xSemaphoreGive(m.done);
                    break;
            }
        }

        uint32_t now = millis32();
        // カードの抜き差し
        if (s_stopped) {
            // シャットダウン後は何もしない
        } else if (g_state.sd_mounted.load() && now - last_card >= kCardCheckMs) {
            esp_err_t st;
            {
                SpiLockGuard lock;
                st = sdmmc_get_status(s_card);
            }
            if (st != ESP_OK) {
                on_removed();
                event_log("SDカードが抜かれました");
            }
            last_card = now;
        } else if (!g_state.sd_mounted.load() && now - last_card >= kRemountMs) {
            if (try_mount(true)) event_log("SDカードを認識しました");
            last_card = now;
            last_free = now;
        }

        // 定期的に fsync して、電源が切れても直前までのファイルが残るようにする
        if (fp && dirty && now - last_sync >= kFsyncIntervalMs) {
            SpiLockGuard lock;
            fflush(fp);
            fsync(fileno(fp));
            dirty = false;
            last_sync = now;
        }
        if (log_fp && log_dirty && now - last_log_sync >= kFsyncIntervalMs) {
            SpiLockGuard lock;
            fflush(log_fp);
            fsync(fileno(log_fp));
            log_dirty = false;
            last_log_sync = now;
        }
        if (g_state.sd_mounted.load() && now - last_free >= kFreeSpaceIntervalMs) {
            update_free_space();
            last_free = now;
        }
    }
}

}  // namespace

bool mount() { return try_mount(false); }

void start_writer_task() {
    s_queue = xQueueCreate(kQueueDepth, sizeof(Msg));
    xTaskCreatePinnedToCore(writer_task, "sd_writer", 4096, nullptr, 4, nullptr, 0);
}

bool open_file(const char* path) {
    Msg m = {};
    m.type = MsgType::Open;
    strlcpy(m.path, path, sizeof(m.path));
    return enqueue(m);
}

bool write(const uint8_t* a, size_t a_len, const uint8_t* b, size_t b_len) {
    Msg m = {};
    m.type = MsgType::Data;
    m.len = a_len + b_len;
    m.data = static_cast<uint8_t*>(heap_caps_malloc(m.len, MALLOC_CAP_SPIRAM));
    if (!m.data) {
        g_state.write_queue_drops.fetch_add(1);
        return false;
    }
    memcpy(m.data, a, a_len);
    if (b_len) memcpy(m.data + a_len, b, b_len);
    if (!enqueue(m)) {
        heap_caps_free(m.data);
        return false;
    }
    return true;
}

bool close_file() {
    Msg m = {};
    m.type = MsgType::Close;
    return enqueue(m);
}

void append_log(const char* line) {
    // 録音の書き込みとは別に数えたいので、失敗しても落ちた数には入れない
    if (!s_queue || s_stopped || !g_state.log_to_sd.load() || !g_state.sd_mounted.load()) return;
    Msg m = {};
    m.type = MsgType::Log;
    m.len = strlen(line);
    m.data = static_cast<uint8_t*>(heap_caps_malloc(m.len, MALLOC_CAP_SPIRAM));
    if (!m.data) return;
    memcpy(m.data, line, m.len);
    if (xQueueSend(s_queue, &m, 0) != pdTRUE) heap_caps_free(m.data);
}

void flush_log() {
    if (!s_queue || s_stopped) return;
    Msg m = {};
    m.type = MsgType::FlushLog;
    xQueueSend(s_queue, &m, 0);
}

bool shutdown(uint32_t timeout_ms) {
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) return false;
    Msg m = {};
    m.type = MsgType::Shutdown;
    m.done = done;
    bool ok = s_queue && xQueueSend(s_queue, &m, pdMS_TO_TICKS(timeout_ms)) == pdTRUE &&
              xSemaphoreTake(done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    vSemaphoreDelete(done);
    return ok;
}

}  // namespace storage
