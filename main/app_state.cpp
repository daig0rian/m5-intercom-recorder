#include "app_state.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

AppState g_state;

static const char* TAG = "event";

namespace {
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
char s_peer_name[64] = "";
char s_file_name[48] = "";

// 直近 kEventLogLines 行を PSRAM に持つ（約 48KB）。古いものから上書きする
char (*s_events)[kEventLineLen] = nullptr;
int s_event_head = 0;   // 次に書く位置
int s_event_count = 0;
uint32_t s_event_total = 0;  // これまでに書いた行数
void (*s_sink)(const char*) = nullptr;

void copy_locked(char* dst, size_t dst_len, const char* src) {
    taskENTER_CRITICAL(&s_lock);
    strlcpy(dst, src ? src : "", dst_len);
    taskEXIT_CRITICAL(&s_lock);
}
}  // namespace

uint32_t millis32() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void state_set_peer_name(const char* name) { copy_locked(s_peer_name, sizeof(s_peer_name), name); }
void state_get_peer_name(char* out, size_t len) { copy_locked(out, len, s_peer_name); }
void state_set_file_name(const char* name) { copy_locked(s_file_name, sizeof(s_file_name), name); }
void state_get_file_name(char* out, size_t len) { copy_locked(out, len, s_file_name); }

void event_log(const char* fmt, ...) {
    char line[kEventLineLen];
    // 時刻は RTC で合わせた本体の時計（起動時に rtc_clock::init が合わせる）
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    int n = snprintf(line, sizeof(line), "%02d:%02d:%02d ", t.tm_hour, t.tm_min, t.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);
    // 切り詰めで UTF-8 の文字が途中で切れていたら、その文字ごと落とす
    size_t len = strlen(line);
    size_t cut = len;
    while (cut > 0 && (static_cast<unsigned char>(line[cut - 1]) & 0xC0) == 0x80) cut--;
    if (cut > 0 && (static_cast<unsigned char>(line[cut - 1]) & 0x80)) {
        unsigned char lead = line[cut - 1];
        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (len - (cut - 1) < need) line[cut - 1] = 0;
    }
    ESP_LOGI(TAG, "%s", line);

    if (s_events) {
        taskENTER_CRITICAL(&s_lock);
        strlcpy(s_events[s_event_head], line, kEventLineLen);
        s_event_head = (s_event_head + 1) % kEventLogLines;
        if (s_event_count < kEventLogLines) s_event_count++;
        s_event_total++;
        taskEXIT_CRITICAL(&s_lock);
    }
    if (s_sink) s_sink(line);
}

void event_log_init(void (*sink)(const char* line)) {
    if (!s_events) {
        s_events = static_cast<char(*)[kEventLineLen]>(
            heap_caps_calloc(kEventLogLines, kEventLineLen, MALLOC_CAP_SPIRAM));
    }
    s_sink = sink;
}

int event_log_read(int start, int n, char lines[][kEventLineLen]) {
    if (!s_events) return 0;
    taskENTER_CRITICAL(&s_lock);
    int oldest = (s_event_head - s_event_count + kEventLogLines) % kEventLogLines;
    if (start < 0) start = 0;
    if (start + n > s_event_count) n = s_event_count - start;
    for (int i = 0; i < n; i++) {
        strlcpy(lines[i], s_events[(oldest + start + i) % kEventLogLines], kEventLineLen);
    }
    taskEXIT_CRITICAL(&s_lock);
    return n > 0 ? n : 0;
}

int event_log_snapshot(char lines[][kEventLineLen], int max_lines) {
    int count = event_log_count();
    int n = count < max_lines ? count : max_lines;
    return event_log_read(count - n, n, lines);
}

int event_log_count() {
    taskENTER_CRITICAL(&s_lock);
    int n = s_event_count;
    taskEXIT_CRITICAL(&s_lock);
    return n;
}

uint32_t event_log_total() {
    taskENTER_CRITICAL(&s_lock);
    uint32_t n = s_event_total;
    taskEXIT_CRITICAL(&s_lock);
    return n;
}
