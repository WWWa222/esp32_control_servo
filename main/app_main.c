#include "alert_service.h"
#include "console_task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_api.h"
#include "ota_service.h"
#include "sdkconfig.h"
#include "servo_control.h"
#include "status_store.h"
#include "wifi_service.h"

static const char *TAG = "app_main";

static void supervisor_task(void *arg)
{
#if !CONFIG_ESP_TASK_WDT_INIT
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = (1 << CONFIG_FREERTOS_NUMBER_OF_CORES) - 1,
        .trigger_panic = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_init(&twdt_config));
#endif

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    int64_t last_status_log_us = 0;
    while (true) {
        const int64_t now_us = esp_timer_get_time();
        app_status_snapshot_t snapshot;

        esp_task_wdt_reset();
        status_store_snapshot(&snapshot);

        if (snapshot.last_heartbeat_age_ms >= 0 &&
            snapshot.last_heartbeat_age_ms > (int64_t)CONFIG_RPC_HOST_HEARTBEAT_TIMEOUT_SEC * 1000) {
            if (status_store_mark_heartbeat_timeout_if_needed("host_heartbeat_timeout")) {
                ESP_LOGW(TAG, "Host heartbeat timeout detected");
                alert_service_send_webhook("host_heartbeat_timeout",
                                           "No heartbeat was received before the timeout window");
            }
        }

        if ((now_us - last_status_log_us) >= (int64_t)CONFIG_RPC_STATUS_LOG_INTERVAL_SEC * 1000000LL) {
            ESP_LOGI(TAG,
                     "status wifi=%s ip=%s servo=%d host_online=%s hb_age_ms=%lld",
                     snapshot.wifi_connected ? "up" : "down",
                     snapshot.ip_address,
                     snapshot.servo_angle,
                     snapshot.host_online ? "yes" : "no",
                     snapshot.last_heartbeat_age_ms);
            last_status_log_us = now_us;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting %s", CONFIG_RPC_DEVICE_NAME);

    status_store_init();
    ESP_ERROR_CHECK(wifi_service_init());
    ESP_ERROR_CHECK(servo_control_init());
    ESP_ERROR_CHECK(ota_service_init());
    status_store_set_servo_angle(servo_control_get_angle());

    esp_err_t wifi_err = wifi_service_start();
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi startup skipped or delayed: %s", esp_err_to_name(wifi_err));
    }

    ESP_ERROR_CHECK(http_api_start());
    ESP_ERROR_CHECK(ota_service_mark_running_app_valid_if_needed());
    console_task_start();
    xTaskCreate(supervisor_task, "supervisor_task", 6144, NULL, 6, NULL);

    ESP_LOGI(TAG, "Startup complete");
}
