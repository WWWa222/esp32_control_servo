#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool wifi_connected;
    char ip_address[16];
    int servo_angle;
    bool host_online;
    uint32_t press_count;
    uint32_t last_press_ms;
    int64_t last_heartbeat_age_ms;
    char host_name[32];
    char user_name[32];
    float cpu_pct;
    float memory_pct;
    uint32_t host_uptime_s;
    char report_source[32];
    char last_report[512];
    int64_t last_alert_age_ms;
    char last_alert_reason[64];
} app_status_snapshot_t;

void status_store_init(void);
void status_store_set_wifi(bool connected, const char *ip_address);
void status_store_set_servo_angle(int angle);
void status_store_record_press(uint32_t press_ms);
void status_store_record_heartbeat(const char *host_name,
                                   const char *user_name,
                                   float cpu_pct,
                                   float memory_pct,
                                   uint32_t uptime_s);
void status_store_record_report(const char *source, const char *payload);
bool status_store_mark_heartbeat_timeout_if_needed(const char *reason);
void status_store_record_alert(const char *reason);
void status_store_snapshot(app_status_snapshot_t *snapshot);
