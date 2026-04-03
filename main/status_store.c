#include "status_store.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    SemaphoreHandle_t lock;
    bool wifi_connected;
    char ip_address[16];
    int servo_angle;
    bool host_online;
    bool heartbeat_timeout_reported;
    uint32_t press_count;
    uint32_t last_press_ms;
    int64_t last_heartbeat_us;
    char host_name[32];
    char user_name[32];
    float cpu_pct;
    float memory_pct;
    uint32_t host_uptime_s;
    char report_source[32];
    char last_report[512];
    int64_t last_alert_us;
    char last_alert_reason[64];
} app_status_state_t;

static app_status_state_t s_state;

static void copy_string(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    strlcpy(dst, src, dst_len);
}

void status_store_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.lock = xSemaphoreCreateMutex();
}

void status_store_set_wifi(bool connected, const char *ip_address)
{
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    s_state.wifi_connected = connected;
    copy_string(s_state.ip_address, sizeof(s_state.ip_address), connected ? ip_address : "");
    xSemaphoreGive(s_state.lock);
}

void status_store_set_servo_angle(int angle)
{
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    s_state.servo_angle = angle;
    xSemaphoreGive(s_state.lock);
}

void status_store_record_press(uint32_t press_ms)
{
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    s_state.press_count++;
    s_state.last_press_ms = press_ms;
    xSemaphoreGive(s_state.lock);
}

void status_store_record_heartbeat(const char *host_name,
                                   const char *user_name,
                                   float cpu_pct,
                                   float memory_pct,
                                   uint32_t uptime_s)
{
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    s_state.host_online = true;
    s_state.heartbeat_timeout_reported = false;
    s_state.last_heartbeat_us = esp_timer_get_time();
    copy_string(s_state.host_name, sizeof(s_state.host_name), host_name);
    copy_string(s_state.user_name, sizeof(s_state.user_name), user_name);
    s_state.cpu_pct = cpu_pct;
    s_state.memory_pct = memory_pct;
    s_state.host_uptime_s = uptime_s;
    copy_string(s_state.report_source, sizeof(s_state.report_source), "legacy_fields");
    snprintf(s_state.last_report,
             sizeof(s_state.last_report),
             "host=%s | user=%s | cpu=%.1f | mem=%.1f | uptime_s=%lu",
             s_state.host_name,
             s_state.user_name,
             (double)cpu_pct,
             (double)memory_pct,
             (unsigned long)uptime_s);
    s_state.last_report[sizeof(s_state.last_report) - 1] = '\0';
    xSemaphoreGive(s_state.lock);
}

void status_store_record_report(const char *source, const char *payload)
{
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    s_state.host_online = true;
    s_state.heartbeat_timeout_reported = false;
    s_state.last_heartbeat_us = esp_timer_get_time();
    copy_string(s_state.report_source, sizeof(s_state.report_source), source);
    copy_string(s_state.last_report, sizeof(s_state.last_report), payload);
    copy_string(s_state.host_name, sizeof(s_state.host_name), source);
    s_state.user_name[0] = '\0';
    s_state.cpu_pct = 0.0f;
    s_state.memory_pct = 0.0f;
    s_state.host_uptime_s = 0;
    xSemaphoreGive(s_state.lock);
}

bool status_store_mark_heartbeat_timeout_if_needed(const char *reason)
{
    bool should_report = false;

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    if (s_state.last_heartbeat_us > 0 && !s_state.heartbeat_timeout_reported) {
        s_state.host_online = false;
        s_state.heartbeat_timeout_reported = true;
        s_state.last_alert_us = esp_timer_get_time();
        copy_string(s_state.last_alert_reason, sizeof(s_state.last_alert_reason), reason);
        should_report = true;
    }
    xSemaphoreGive(s_state.lock);

    return should_report;
}

void status_store_record_alert(const char *reason)
{
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    s_state.last_alert_us = esp_timer_get_time();
    copy_string(s_state.last_alert_reason, sizeof(s_state.last_alert_reason), reason);
    xSemaphoreGive(s_state.lock);
}

void status_store_snapshot(app_status_snapshot_t *snapshot)
{
    const int64_t now_us = esp_timer_get_time();

    memset(snapshot, 0, sizeof(*snapshot));

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    snapshot->wifi_connected = s_state.wifi_connected;
    copy_string(snapshot->ip_address, sizeof(snapshot->ip_address), s_state.ip_address);
    snapshot->servo_angle = s_state.servo_angle;
    snapshot->host_online = s_state.host_online;
    snapshot->press_count = s_state.press_count;
    snapshot->last_press_ms = s_state.last_press_ms;
    copy_string(snapshot->host_name, sizeof(snapshot->host_name), s_state.host_name);
    copy_string(snapshot->user_name, sizeof(snapshot->user_name), s_state.user_name);
    snapshot->cpu_pct = s_state.cpu_pct;
    snapshot->memory_pct = s_state.memory_pct;
    snapshot->host_uptime_s = s_state.host_uptime_s;
    copy_string(snapshot->report_source, sizeof(snapshot->report_source), s_state.report_source);
    copy_string(snapshot->last_report, sizeof(snapshot->last_report), s_state.last_report);
    copy_string(snapshot->last_alert_reason, sizeof(snapshot->last_alert_reason), s_state.last_alert_reason);
    snapshot->last_heartbeat_age_ms = (s_state.last_heartbeat_us > 0)
                                          ? (now_us - s_state.last_heartbeat_us) / 1000
                                          : -1;
    snapshot->last_alert_age_ms = (s_state.last_alert_us > 0)
                                      ? (now_us - s_state.last_alert_us) / 1000
                                      : -1;
    xSemaphoreGive(s_state.lock);
}
