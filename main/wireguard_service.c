#include "wireguard_service.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wireguard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wifi_service.h"

#ifndef CONFIG_RPC_WIREGUARD_ENABLE
#define CONFIG_RPC_WIREGUARD_ENABLE 0
#endif

#ifndef CONFIG_RPC_WIREGUARD_PRIVATE_KEY
#define CONFIG_RPC_WIREGUARD_PRIVATE_KEY ""
#endif

#ifndef CONFIG_RPC_WIREGUARD_PEER_PUBLIC_KEY
#define CONFIG_RPC_WIREGUARD_PEER_PUBLIC_KEY ""
#endif

#ifndef CONFIG_RPC_WIREGUARD_PRESHARED_KEY
#define CONFIG_RPC_WIREGUARD_PRESHARED_KEY ""
#endif

#ifndef CONFIG_RPC_WIREGUARD_LOCAL_IP
#define CONFIG_RPC_WIREGUARD_LOCAL_IP ""
#endif

#ifndef CONFIG_RPC_WIREGUARD_LOCAL_MASK
#define CONFIG_RPC_WIREGUARD_LOCAL_MASK ""
#endif

#ifndef CONFIG_RPC_WIREGUARD_ENDPOINT
#define CONFIG_RPC_WIREGUARD_ENDPOINT ""
#endif

#ifndef CONFIG_RPC_WIREGUARD_ENDPOINT_PORT
#define CONFIG_RPC_WIREGUARD_ENDPOINT_PORT 51820
#endif

#ifndef CONFIG_RPC_WIREGUARD_LISTEN_PORT
#define CONFIG_RPC_WIREGUARD_LISTEN_PORT 51820
#endif

#ifndef CONFIG_RPC_WIREGUARD_KEEPALIVE_SEC
#define CONFIG_RPC_WIREGUARD_KEEPALIVE_SEC 25
#endif

#ifndef CONFIG_RPC_WIREGUARD_SNTP_SERVER
#define CONFIG_RPC_WIREGUARD_SNTP_SERVER "time.cloudflare.com"
#endif

#define WG_VALID_UNIX_TIME          1704067200LL
#define WG_RETRY_DELAY_MS           5000
#define WG_POLL_DELAY_DOWN_MS       2000
#define WG_POLL_DELAY_UP_MS         5000
#define WG_SNTP_TIMEOUT_MS          15000

static const char *TAG = "wireguard_service";

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    wireguard_ctx_t ctx;
    wireguard_config_t config;
    wireguard_service_status_t status;
    bool initialized;
    bool task_started;
    bool ctx_ready;
    bool active;
    bool force_reconnect;
} wireguard_service_state_t;

static wireguard_service_state_t s_state;

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

static void set_last_error_locked(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_state.status.last_error, sizeof(s_state.status.last_error), fmt, args);
    va_end(args);
}

static void clear_last_error_locked(void)
{
    s_state.status.last_error[0] = '\0';
}

static bool has_valid_time(void)
{
    time_t now = 0;
    time(&now);
    return now >= WG_VALID_UNIX_TIME;
}

static bool wireguard_has_required_config(void)
{
    return strlen(CONFIG_RPC_WIREGUARD_PRIVATE_KEY) > 0 &&
           strlen(CONFIG_RPC_WIREGUARD_PEER_PUBLIC_KEY) > 0 &&
           strlen(CONFIG_RPC_WIREGUARD_LOCAL_IP) > 0 &&
           strlen(CONFIG_RPC_WIREGUARD_LOCAL_MASK) > 0 &&
           strlen(CONFIG_RPC_WIREGUARD_ENDPOINT) > 0;
}

static esp_err_t sync_time_if_needed(void)
{
    if (has_valid_time()) {
        return ESP_OK;
    }

    esp_netif_sntp_deinit();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_RPC_WIREGUARD_SNTP_SERVER);
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(WG_SNTP_TIMEOUT_MS));
    esp_netif_sntp_deinit();
    return err;
}

static void fill_runtime_config_locked(void)
{
    s_state.config = (wireguard_config_t)ESP_WIREGUARD_CONFIG_DEFAULT();
    s_state.config.private_key = (char *)CONFIG_RPC_WIREGUARD_PRIVATE_KEY;
    s_state.config.listen_port = CONFIG_RPC_WIREGUARD_LISTEN_PORT;
    s_state.config.public_key = (char *)CONFIG_RPC_WIREGUARD_PEER_PUBLIC_KEY;
    s_state.config.preshared_key =
        (strlen(CONFIG_RPC_WIREGUARD_PRESHARED_KEY) > 0) ? (char *)CONFIG_RPC_WIREGUARD_PRESHARED_KEY : NULL;
    s_state.config.allowed_ip = (char *)CONFIG_RPC_WIREGUARD_LOCAL_IP;
    s_state.config.allowed_ip_mask = (char *)CONFIG_RPC_WIREGUARD_LOCAL_MASK;
    s_state.config.endpoint = (char *)CONFIG_RPC_WIREGUARD_ENDPOINT;
    s_state.config.port = CONFIG_RPC_WIREGUARD_ENDPOINT_PORT;
    s_state.config.persistent_keepalive = CONFIG_RPC_WIREGUARD_KEEPALIVE_SEC;
}

static esp_err_t ensure_context_ready_locked(void)
{
    if (s_state.ctx_ready) {
        return ESP_OK;
    }

    fill_runtime_config_locked();
    memset(&s_state.ctx, 0, sizeof(s_state.ctx));
    esp_err_t err = esp_wireguard_init(&s_state.config, &s_state.ctx);
    if (err != ESP_OK) {
        set_last_error_locked("wg_init:%s", esp_err_to_name(err));
        return err;
    }

    s_state.ctx_ready = true;
    return ESP_OK;
}

static void disconnect_tunnel_locked(const char *reason)
{
    if (s_state.active && s_state.ctx.netif != NULL) {
        esp_err_t err = esp_wireguard_disconnect(&s_state.ctx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "WireGuard disconnect failed: %s", esp_err_to_name(err));
        }
    }

    s_state.active = false;
    s_state.status.active = false;
    s_state.status.peer_up = false;
    if (reason != NULL && reason[0] != '\0') {
        set_last_error_locked("%s", reason);
    }
}

static esp_err_t connect_tunnel_locked(void)
{
    esp_err_t err = ensure_context_ready_locked();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wireguard_connect(&s_state.ctx);
    if (err != ESP_OK) {
        set_last_error_locked("wg_connect:%s", esp_err_to_name(err));
        return err;
    }

    s_state.active = true;
    s_state.status.active = true;
    s_state.status.peer_up = false;
    clear_last_error_locked();
    return ESP_OK;
}

static void update_static_status_locked(void)
{
    s_state.status.enabled = CONFIG_RPC_WIREGUARD_ENABLE;
    s_state.status.configured = wireguard_has_required_config();
    s_state.status.endpoint_port = CONFIG_RPC_WIREGUARD_ENDPOINT_PORT;
    s_state.status.keepalive_sec = CONFIG_RPC_WIREGUARD_KEEPALIVE_SEC;
    copy_string(s_state.status.local_ip, sizeof(s_state.status.local_ip), CONFIG_RPC_WIREGUARD_LOCAL_IP);
    copy_string(s_state.status.local_mask, sizeof(s_state.status.local_mask), CONFIG_RPC_WIREGUARD_LOCAL_MASK);
    copy_string(s_state.status.endpoint, sizeof(s_state.status.endpoint), CONFIG_RPC_WIREGUARD_ENDPOINT);
}

static void wireguard_task(void *arg)
{
    while (true) {
        const bool wifi_connected = wifi_service_is_connected();
        bool should_reconnect = false;

        xSemaphoreTake(s_state.lock, portMAX_DELAY);
        update_static_status_locked();
        s_state.status.waiting_for_network = !wifi_connected;

        if (!CONFIG_RPC_WIREGUARD_ENABLE) {
            clear_last_error_locked();
            xSemaphoreGive(s_state.lock);
            vTaskDelay(pdMS_TO_TICKS(WG_POLL_DELAY_UP_MS));
            continue;
        }

        if (!wireguard_has_required_config()) {
            s_state.status.time_synced = has_valid_time();
            disconnect_tunnel_locked("wg_config_missing");
            xSemaphoreGive(s_state.lock);
            vTaskDelay(pdMS_TO_TICKS(WG_RETRY_DELAY_MS));
            continue;
        }

        if (!wifi_connected) {
            s_state.status.time_synced = has_valid_time();
            disconnect_tunnel_locked("waiting_for_wifi");
            xSemaphoreGive(s_state.lock);
            vTaskDelay(pdMS_TO_TICKS(WG_RETRY_DELAY_MS));
            continue;
        }

        should_reconnect = s_state.force_reconnect || !s_state.active;
        s_state.force_reconnect = false;
        xSemaphoreGive(s_state.lock);

        esp_err_t err = sync_time_if_needed();

        xSemaphoreTake(s_state.lock, portMAX_DELAY);
        s_state.status.time_synced = (err == ESP_OK) || has_valid_time();
        if (err != ESP_OK) {
            disconnect_tunnel_locked("time_sync_failed");
            xSemaphoreGive(s_state.lock);
            ESP_LOGW(TAG, "SNTP sync failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(WG_RETRY_DELAY_MS));
            continue;
        }

        if (should_reconnect) {
            if (s_state.active) {
                disconnect_tunnel_locked("reconnecting");
            }

            err = connect_tunnel_locked();
            if (err != ESP_OK) {
                xSemaphoreGive(s_state.lock);
                ESP_LOGW(TAG, "WireGuard connect attempt failed: %s", esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(WG_RETRY_DELAY_MS));
                continue;
            }

            ESP_LOGI(TAG,
                     "WireGuard connect requested endpoint=%s:%d local_ip=%s",
                     s_state.status.endpoint,
                     s_state.status.endpoint_port,
                     s_state.status.local_ip);
        }

        if (s_state.active && s_state.ctx.netif != NULL) {
            if (esp_wireguardif_peer_is_up(&s_state.ctx) == ESP_OK) {
                s_state.status.peer_up = true;
                clear_last_error_locked();
            } else {
                s_state.status.peer_up = false;
                if (s_state.status.last_error[0] == '\0' ||
                    strcmp(s_state.status.last_error, "peer_down") == 0) {
                    set_last_error_locked("peer_down");
                }
            }
        } else {
            s_state.status.peer_up = false;
        }

        const bool peer_up = s_state.status.peer_up;
        xSemaphoreGive(s_state.lock);
        vTaskDelay(pdMS_TO_TICKS(peer_up ? WG_POLL_DELAY_UP_MS : WG_POLL_DELAY_DOWN_MS));
    }
}

esp_err_t wireguard_service_init(void)
{
    if (s_state.initialized) {
        return ESP_OK;
    }

    s_state.lock = xSemaphoreCreateMutex();
    if (s_state.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    update_static_status_locked();
    s_state.status.time_synced = has_valid_time();
    if (!CONFIG_RPC_WIREGUARD_ENABLE) {
        clear_last_error_locked();
    } else if (!wireguard_has_required_config()) {
        set_last_error_locked("wg_config_missing");
    }
    xSemaphoreGive(s_state.lock);

    s_state.initialized = true;
    return ESP_OK;
}

void wireguard_service_start(void)
{
    if (!s_state.initialized) {
        if (wireguard_service_init() != ESP_OK) {
            return;
        }
    }

    if (s_state.task_started) {
        return;
    }

    if (xTaskCreate(wireguard_task, "wireguard_task", 8192, NULL, 4, &s_state.task) != pdPASS) {
        ESP_LOGE(TAG, "Unable to create WireGuard task");
        return;
    }

    s_state.task_started = true;
}

esp_err_t wireguard_service_force_disconnect(void)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    disconnect_tunnel_locked("manual_disconnect");
    xSemaphoreGive(s_state.lock);
    return ESP_OK;
}

esp_err_t wireguard_service_force_reconnect(void)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    s_state.force_reconnect = true;
    xSemaphoreGive(s_state.lock);
    return ESP_OK;
}

void wireguard_service_get_status(wireguard_service_status_t *status)
{
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));

    if (!s_state.initialized || s_state.lock == NULL) {
        status->enabled = CONFIG_RPC_WIREGUARD_ENABLE;
        return;
    }

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    *status = s_state.status;
    xSemaphoreGive(s_state.lock);
}

bool wireguard_service_is_enabled(void)
{
    return CONFIG_RPC_WIREGUARD_ENABLE;
}
