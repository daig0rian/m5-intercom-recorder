#include "rtc_clock.h"

#include <sys/time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "M5Unified.h"
#include "app_state.h"
#include "esp_log.h"
#include "spi_lock.h"
#include "storage.h"

static const char* TAG = "clock";

namespace rtc_clock {
namespace {

constexpr int kMinValidYear = 2025;

void set_system_time(const struct tm& local) {
    struct tm t = local;
    t.tm_isdst = 0;
    time_t epoch = mktime(&t);
    struct timeval tv = {epoch, 0};
    settimeofday(&tv, nullptr);
}

void write_rtc(const struct tm& t) {
    m5::rtc_datetime_t dt;
    dt.date.year = t.tm_year + 1900;
    dt.date.month = t.tm_mon + 1;
    dt.date.date = t.tm_mday;
    dt.date.weekDay = t.tm_wday;
    dt.time.hours = t.tm_hour;
    dt.time.minutes = t.tm_min;
    dt.time.seconds = t.tm_sec;
    M5.Rtc.setDateTime(dt);
}

bool parse_datetime(const char* s, struct tm* out) {
    int y, mo, d, h, mi, sec = 0;
    if (sscanf(s, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 5) return false;
    if (y < kMinValidYear || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 59) return false;
    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = sec;
    mktime(&t);  // 曜日を埋める
    *out = t;
    return true;
}

// SD カードの settime.txt があれば読む
bool read_settime_file(struct tm* out) {
    if (!g_state.sd_mounted.load()) return false;
    char path[32], done[32];
    snprintf(path, sizeof(path), "%s/settime.txt", storage::kMountPoint);
    snprintf(done, sizeof(done), "%s/settime.done", storage::kMountPoint);
    char line[64] = {};
    bool ok = false;
    {
        SpiLockGuard lock;
        FILE* fp = fopen(path, "r");
        if (!fp) return false;
        ok = fgets(line, sizeof(line), fp) != nullptr;
        fclose(fp);
        if (ok) {
            remove(done);
            rename(path, done);
        }
    }
    if (!ok || !parse_datetime(line, out)) {
        ESP_LOGW(TAG, "settime.txt を読めませんでした: %s", line);
        return false;
    }
    return true;
}

// ビルドした日時（__DATE__ は "Nov 20 2026"、__TIME__ は "10:30:00"）
bool build_time(struct tm* out) {
    static const char* kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {};
    int d, y, h, mi, s;
    if (sscanf(__DATE__, "%3s %d %d", mon, &d, &y) != 3) return false;
    if (sscanf(__TIME__, "%d:%d:%d", &h, &mi, &s) != 3) return false;
    const char* p = strstr(kMonths, mon);
    if (!p) return false;
    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = (p - kMonths) / 3;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
    mktime(&t);
    *out = t;
    return true;
}

}  // namespace

void init() {
    setenv("TZ", "JST-9", 1);
    tzset();

    struct tm t = {};
    if (read_settime_file(&t)) {
        write_rtc(t);
        set_system_time(t);
        event_log("時刻を設定 %04d-%02d-%02d %02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                  t.tm_min);
        return;
    }

    if (M5.Rtc.isEnabled()) {
        auto dt = M5.Rtc.getDateTime();
        if (dt.date.year >= kMinValidYear) {
            t.tm_year = dt.date.year - 1900;
            t.tm_mon = dt.date.month - 1;
            t.tm_mday = dt.date.date;
            t.tm_hour = dt.time.hours;
            t.tm_min = dt.time.minutes;
            t.tm_sec = dt.time.seconds;
            set_system_time(t);
            return;
        }
    }

    // RTC が合っていない（電池切れなど）。とりあえずビルド日時にしておく
    if (build_time(&t)) {
        ESP_LOGW(TAG, "RTC の時刻が無効なので、ビルド日時を使います");
        if (M5.Rtc.isEnabled()) write_rtc(t);
        set_system_time(t);
        event_log("RTC未設定: settime.txt で合わせてください");
    }
}

void local_now(struct tm* out) {
    time_t now = time(nullptr);
    localtime_r(&now, out);
}

}  // namespace rtc_clock
