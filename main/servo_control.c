#include "servo_control.h"

#include <stdbool.h>
#include <string.h>

#include "driver/mcpwm_prelude.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#define SERVO_TIMEBASE_RESOLUTION_HZ 1000000
#define SERVO_TIMEBASE_PERIOD_TICKS  20000

#define SERVO_NVS_NAMESPACE "rpc_servo"
#define SERVO_NVS_CONFIG_KEY "config"
#define SERVO_NVS_VERSION    1U

static const char *TAG = "servo_control";

#ifndef CONFIG_RPC_SERVO_READY_ANGLE
#define CONFIG_RPC_SERVO_READY_ANGLE CONFIG_RPC_SERVO_REST_ANGLE
#endif

#ifndef CONFIG_RPC_SERVO_SHORT_PRESS_ANGLE
#define CONFIG_RPC_SERVO_SHORT_PRESS_ANGLE CONFIG_RPC_SERVO_PRESS_ANGLE
#endif

#ifndef CONFIG_RPC_SERVO_LONG_PRESS_ANGLE
#define CONFIG_RPC_SERVO_LONG_PRESS_ANGLE CONFIG_RPC_SERVO_PRESS_ANGLE
#endif

#ifndef CONFIG_RPC_SERVO_PREPARE_SETTLE_MS
#define CONFIG_RPC_SERVO_PREPARE_SETTLE_MS 150
#endif

#ifndef CONFIG_RPC_SERVO_RELEASE_SETTLE_MS
#define CONFIG_RPC_SERVO_RELEASE_SETTLE_MS 150
#endif

typedef struct {
    uint32_t version;
    int32_t rest_angle;
    int32_t ready_angle;
    int32_t short_press_angle;
    int32_t long_press_angle;
    uint32_t short_press_ms;
    uint32_t long_press_ms;
    uint32_t prepare_settle_ms;
    uint32_t release_settle_ms;
} servo_runtime_config_storage_t;

static SemaphoreHandle_t s_servo_lock;
static mcpwm_timer_handle_t s_timer;
static mcpwm_oper_handle_t s_operator;
static mcpwm_cmpr_handle_t s_comparator;
static mcpwm_gen_handle_t s_generator;
static int s_current_angle;
static servo_control_config_t s_config;

static int clamp_angle(int angle)
{
    if (angle < CONFIG_RPC_SERVO_MIN_ANGLE) {
        return CONFIG_RPC_SERVO_MIN_ANGLE;
    }
    if (angle > CONFIG_RPC_SERVO_MAX_ANGLE) {
        return CONFIG_RPC_SERVO_MAX_ANGLE;
    }
    return angle;
}

static servo_control_config_t default_config(void)
{
    servo_control_config_t config = {
        .min_angle = CONFIG_RPC_SERVO_MIN_ANGLE,
        .max_angle = CONFIG_RPC_SERVO_MAX_ANGLE,
        .rest_angle = clamp_angle(CONFIG_RPC_SERVO_REST_ANGLE),
        .ready_angle = clamp_angle(CONFIG_RPC_SERVO_READY_ANGLE),
        .short_press_angle = clamp_angle(CONFIG_RPC_SERVO_SHORT_PRESS_ANGLE),
        .long_press_angle = clamp_angle(CONFIG_RPC_SERVO_LONG_PRESS_ANGLE),
        .short_press_ms = CONFIG_RPC_SHORT_PRESS_MS,
        .long_press_ms = CONFIG_RPC_LONG_PRESS_MS,
        .prepare_settle_ms = CONFIG_RPC_SERVO_PREPARE_SETTLE_MS,
        .release_settle_ms = CONFIG_RPC_SERVO_RELEASE_SETTLE_MS,
    };

    return config;
}

static esp_err_t normalize_config(const servo_control_config_t *input, servo_control_config_t *output)
{
    if (input == NULL || output == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    servo_control_config_t normalized = default_config();
    normalized.rest_angle = clamp_angle(input->rest_angle);
    normalized.ready_angle = clamp_angle(input->ready_angle);
    normalized.short_press_angle = clamp_angle(input->short_press_angle);
    normalized.long_press_angle = clamp_angle(input->long_press_angle);
    normalized.short_press_ms = input->short_press_ms;
    normalized.long_press_ms = input->long_press_ms;
    normalized.prepare_settle_ms = input->prepare_settle_ms;
    normalized.release_settle_ms = input->release_settle_ms;

    if (normalized.short_press_ms == 0 || normalized.long_press_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (normalized.prepare_settle_ms > 30000 || normalized.release_settle_ms > 30000) {
        return ESP_ERR_INVALID_ARG;
    }

    *output = normalized;
    return ESP_OK;
}

static uint32_t angle_to_compare_ticks(int angle)
{
    const int bounded = clamp_angle(angle);
    const int angle_span = CONFIG_RPC_SERVO_MAX_ANGLE - CONFIG_RPC_SERVO_MIN_ANGLE;
    const int pulse_span = CONFIG_RPC_SERVO_MAX_PULSE_US - CONFIG_RPC_SERVO_MIN_PULSE_US;

    if (angle_span <= 0) {
        return CONFIG_RPC_SERVO_MIN_PULSE_US;
    }

    return (uint32_t)((bounded - CONFIG_RPC_SERVO_MIN_ANGLE) * pulse_span / angle_span +
                      CONFIG_RPC_SERVO_MIN_PULSE_US);
}

static esp_err_t apply_angle_locked(int angle)
{
    const int bounded = clamp_angle(angle);
    esp_err_t err = mcpwm_comparator_set_compare_value(s_comparator, angle_to_compare_ticks(bounded));
    if (err == ESP_OK) {
        s_current_angle = bounded;
    }
    return err;
}

static esp_err_t save_config_to_nvs(const servo_control_config_t *config)
{
    servo_runtime_config_storage_t storage = {
        .version = SERVO_NVS_VERSION,
        .rest_angle = config->rest_angle,
        .ready_angle = config->ready_angle,
        .short_press_angle = config->short_press_angle,
        .long_press_angle = config->long_press_angle,
        .short_press_ms = config->short_press_ms,
        .long_press_ms = config->long_press_ms,
        .prepare_settle_ms = config->prepare_settle_ms,
        .release_settle_ms = config->release_settle_ms,
    };

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SERVO_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, SERVO_NVS_CONFIG_KEY, &storage, sizeof(storage));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static esp_err_t load_config_from_nvs(servo_control_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(SERVO_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }

    servo_runtime_config_storage_t storage;
    size_t storage_len = sizeof(storage);
    err = nvs_get_blob(handle, SERVO_NVS_CONFIG_KEY, &storage, &storage_len);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (storage_len != sizeof(storage) || storage.version != SERVO_NVS_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }

    servo_control_config_t loaded = default_config();
    loaded.rest_angle = (int)storage.rest_angle;
    loaded.ready_angle = (int)storage.ready_angle;
    loaded.short_press_angle = (int)storage.short_press_angle;
    loaded.long_press_angle = (int)storage.long_press_angle;
    loaded.short_press_ms = storage.short_press_ms;
    loaded.long_press_ms = storage.long_press_ms;
    loaded.prepare_settle_ms = storage.prepare_settle_ms;
    loaded.release_settle_ms = storage.release_settle_ms;

    return normalize_config(&loaded, config);
}

static esp_err_t perform_press_locked(int press_angle, uint32_t press_ms)
{
    const int ready_angle = clamp_angle(s_config.ready_angle);
    const int target_press_angle = clamp_angle(press_angle);
    const int rest_angle = clamp_angle(s_config.rest_angle);
    const bool moved_to_ready = (s_current_angle != ready_angle);
    const bool moved_to_press = (ready_angle != target_press_angle);
    const bool moved_to_rest = (target_press_angle != rest_angle);

    esp_err_t err = ESP_OK;
    if (moved_to_ready) {
        err = apply_angle_locked(ready_angle);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (moved_to_ready && moved_to_press && s_config.prepare_settle_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(s_config.prepare_settle_ms));
    }

    if (s_current_angle != target_press_angle) {
        err = apply_angle_locked(target_press_angle);
    }
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(press_ms));

    if (s_current_angle != rest_angle) {
        err = apply_angle_locked(rest_angle);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (moved_to_rest && s_config.release_settle_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(s_config.release_settle_ms));
    }

    return ESP_OK;
}

esp_err_t servo_control_init(void)
{
    s_servo_lock = xSemaphoreCreateMutex();
    if (s_servo_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    mcpwm_timer_config_t timer_config = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = SERVO_TIMEBASE_RESOLUTION_HZ,
        .period_ticks = SERVO_TIMEBASE_PERIOD_TICKS,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
    };
    ESP_ERROR_CHECK(mcpwm_new_timer(&timer_config, &s_timer));

    mcpwm_operator_config_t operator_config = {
        .group_id = 0,
    };
    ESP_ERROR_CHECK(mcpwm_new_operator(&operator_config, &s_operator));
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(s_operator, s_timer));

    mcpwm_comparator_config_t comparator_config = {
        .flags.update_cmp_on_tez = true,
    };
    ESP_ERROR_CHECK(mcpwm_new_comparator(s_operator, &comparator_config, &s_comparator));

    mcpwm_generator_config_t generator_config = {
        .gen_gpio_num = CONFIG_RPC_SERVO_GPIO,
    };
    ESP_ERROR_CHECK(mcpwm_new_generator(s_operator, &generator_config, &s_generator));
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(
        s_generator,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(
        s_generator,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, s_comparator, MCPWM_GEN_ACTION_LOW)));

    s_config = default_config();
    servo_control_config_t persisted_config;
    if (load_config_from_nvs(&persisted_config) == ESP_OK) {
        s_config = persisted_config;
    }

    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(
        s_comparator, angle_to_compare_ticks(s_config.rest_angle)));
    ESP_ERROR_CHECK(mcpwm_timer_enable(s_timer));
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(s_timer, MCPWM_TIMER_START_NO_STOP));

    s_current_angle = s_config.rest_angle;

    if (s_config.short_press_angle == s_config.rest_angle &&
        s_config.long_press_angle == s_config.rest_angle) {
        ESP_LOGW(TAG,
                 "Servo press angles equal rest angle (%d); short/long press commands will not move the servo",
                 s_config.rest_angle);
    }

    ESP_LOGI(TAG,
             "Servo config rest=%d ready=%d short_angle=%d long_angle=%d short_ms=%lu long_ms=%lu",
             s_config.rest_angle,
             s_config.ready_angle,
             s_config.short_press_angle,
             s_config.long_press_angle,
             (unsigned long)s_config.short_press_ms,
             (unsigned long)s_config.long_press_ms);
    return ESP_OK;
}

esp_err_t servo_control_set_angle(int angle)
{
    xSemaphoreTake(s_servo_lock, portMAX_DELAY);
    esp_err_t err = apply_angle_locked(angle);
    xSemaphoreGive(s_servo_lock);
    return err;
}

esp_err_t servo_control_reset_to_rest(void)
{
    xSemaphoreTake(s_servo_lock, portMAX_DELAY);
    esp_err_t err = apply_angle_locked(s_config.rest_angle);
    xSemaphoreGive(s_servo_lock);
    return err;
}

esp_err_t servo_control_press(uint32_t press_ms)
{
    return servo_control_press_custom(s_config.short_press_angle, press_ms);
}

esp_err_t servo_control_press_mode(servo_control_press_mode_t mode)
{
    int press_angle = 0;
    uint32_t press_ms = 0;

    xSemaphoreTake(s_servo_lock, portMAX_DELAY);
    if (mode == SERVO_CONTROL_PRESS_MODE_LONG) {
        press_angle = s_config.long_press_angle;
        press_ms = s_config.long_press_ms;
    } else {
        press_angle = s_config.short_press_angle;
        press_ms = s_config.short_press_ms;
    }

    esp_err_t err = perform_press_locked(press_angle, press_ms);
    xSemaphoreGive(s_servo_lock);
    return err;
}

esp_err_t servo_control_press_custom(int angle, uint32_t press_ms)
{
    if (press_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_servo_lock, portMAX_DELAY);
    esp_err_t err = perform_press_locked(angle, press_ms);
    xSemaphoreGive(s_servo_lock);
    return err;
}

esp_err_t servo_control_get_config(servo_control_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_servo_lock, portMAX_DELAY);
    *config = s_config;
    xSemaphoreGive(s_servo_lock);
    return ESP_OK;
}

esp_err_t servo_control_update_config(const servo_control_config_t *config, bool persist, bool move_to_rest)
{
    servo_control_config_t normalized;
    esp_err_t err = normalize_config(config, &normalized);
    if (err != ESP_OK) {
        return err;
    }

    if (persist) {
        err = save_config_to_nvs(&normalized);
        if (err != ESP_OK) {
            return err;
        }
    }

    xSemaphoreTake(s_servo_lock, portMAX_DELAY);
    s_config = normalized;
    if (move_to_rest) {
        err = apply_angle_locked(s_config.rest_angle);
    }
    xSemaphoreGive(s_servo_lock);

    return err;
}

int servo_control_get_angle(void)
{
    int angle;

    xSemaphoreTake(s_servo_lock, portMAX_DELAY);
    angle = s_current_angle;
    xSemaphoreGive(s_servo_lock);

    return angle;
}
