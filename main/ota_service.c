#include "ota_service.h"

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_service.h"

static const char *TAG = "ota_service";

typedef struct {
    SemaphoreHandle_t lock;
    bool initialized;
    bool in_progress;
    bool rollback_pending_verify;
    bool last_success;
    esp_err_t last_error;
    char status[32];
    char detail[96];
    char url[192];
} ota_service_state_t;

static ota_service_state_t s_ota_state;

static void ota_copy_string(char *dst, size_t dst_len, const char *src)
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

static void ota_set_status(bool in_progress,
                           bool last_success,
                           esp_err_t last_error,
                           const char *status,
                           const char *detail,
                           const char *url)
{
    xSemaphoreTake(s_ota_state.lock, portMAX_DELAY);
    s_ota_state.in_progress = in_progress;
    s_ota_state.last_success = last_success;
    s_ota_state.last_error = last_error;
    ota_copy_string(s_ota_state.status, sizeof(s_ota_state.status), status);
    ota_copy_string(s_ota_state.detail, sizeof(s_ota_state.detail), detail);
    ota_copy_string(s_ota_state.url, sizeof(s_ota_state.url), url);
    xSemaphoreGive(s_ota_state.lock);
}

static void ota_refresh_pending_verify_flag(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    bool pending_verify = false;

    if (running != NULL && esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        pending_verify = (ota_state == ESP_OTA_IMG_PENDING_VERIFY);
    }

    xSemaphoreTake(s_ota_state.lock, portMAX_DELAY);
    s_ota_state.rollback_pending_verify = pending_verify;
    xSemaphoreGive(s_ota_state.lock);
}

static void ota_task(void *arg)
{
    char url[192];
    ota_copy_string(url, sizeof(url), (const char *)arg);
    vPortFree(arg);

    if (!wifi_service_is_connected()) {
        ota_set_status(false, false, ESP_ERR_INVALID_STATE, "failed", "Wi-Fi is not connected", url);
        vTaskDelete(NULL);
        return;
    }

    ota_set_status(true, false, ESP_OK, "downloading", "Downloading firmware image", url);

    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_err_t err = esp_https_ota(&ota_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed for %s: %s", url, esp_err_to_name(err));
        ota_set_status(false, false, err, "failed", esp_err_to_name(err), url);
        vTaskDelete(NULL);
        return;
    }

    ota_set_status(false, true, ESP_OK, "restarting", "OTA complete, restarting", url);
    ESP_LOGI(TAG, "OTA succeeded, restarting from %s", url);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

esp_err_t ota_service_init(void)
{
    if (s_ota_state.initialized) {
        ota_refresh_pending_verify_flag();
        return ESP_OK;
    }

    memset(&s_ota_state, 0, sizeof(s_ota_state));
    s_ota_state.lock = xSemaphoreCreateMutex();
    if (s_ota_state.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ota_copy_string(s_ota_state.status, sizeof(s_ota_state.status), "idle");
    ota_copy_string(s_ota_state.detail, sizeof(s_ota_state.detail), "OTA not started");
    s_ota_state.initialized = true;
    ota_refresh_pending_verify_flag();
    return ESP_OK;
}

esp_err_t ota_service_mark_running_app_valid_if_needed(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;

    if (running == NULL) {
        return ESP_OK;
    }

    if (esp_ota_get_state_partition(running, &ota_state) != ESP_OK) {
        ota_refresh_pending_verify_flag();
        return ESP_OK;
    }

    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            ota_set_status(false, false, err, "rollback", "Failed to confirm new app", "");
            ota_refresh_pending_verify_flag();
            return err;
        }

        ota_set_status(false, true, ESP_OK, "confirmed", "Running app marked valid", "");
    }

    ota_refresh_pending_verify_flag();
    return ESP_OK;
}

esp_err_t ota_service_start(const char *url)
{
    char *task_url = NULL;

    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ota_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_ota_state.lock, portMAX_DELAY);
    if (s_ota_state.in_progress) {
        xSemaphoreGive(s_ota_state.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ota_state.in_progress = true;
    xSemaphoreGive(s_ota_state.lock);

    task_url = pvPortMalloc(192);
    if (task_url == NULL) {
        ota_set_status(false, false, ESP_ERR_NO_MEM, "failed", "Unable to allocate OTA task buffer", url);
        return ESP_ERR_NO_MEM;
    }

    ota_copy_string(task_url, 192, url);
    ota_set_status(true, false, ESP_OK, "starting", "Preparing OTA download", url);

    if (xTaskCreate(ota_task, "ota_task", 10240, task_url, 5, NULL) != pdPASS) {
        vPortFree(task_url);
        ota_set_status(false, false, ESP_FAIL, "failed", "Unable to start OTA task", url);
        return ESP_FAIL;
    }

    return ESP_OK;
}

void ota_service_get_status(ota_service_status_t *status)
{
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));

    xSemaphoreTake(s_ota_state.lock, portMAX_DELAY);
    status->in_progress = s_ota_state.in_progress;
    status->rollback_pending_verify = s_ota_state.rollback_pending_verify;
    status->last_success = s_ota_state.last_success;
    status->last_error = s_ota_state.last_error;
    ota_copy_string(status->status, sizeof(status->status), s_ota_state.status);
    ota_copy_string(status->detail, sizeof(status->detail), s_ota_state.detail);
    ota_copy_string(status->url, sizeof(status->url), s_ota_state.url);
    xSemaphoreGive(s_ota_state.lock);
}
