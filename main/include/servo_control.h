#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    SERVO_CONTROL_PRESS_MODE_SHORT = 0,
    SERVO_CONTROL_PRESS_MODE_LONG,
} servo_control_press_mode_t;

typedef struct {
    int min_angle;
    int max_angle;
    int rest_angle;
    int ready_angle;
    int short_press_angle;
    int long_press_angle;
    uint32_t short_press_ms;
    uint32_t long_press_ms;
    uint32_t prepare_settle_ms;
    uint32_t release_settle_ms;
} servo_control_config_t;

esp_err_t servo_control_init(void);
esp_err_t servo_control_set_angle(int angle);
esp_err_t servo_control_reset_to_rest(void);
esp_err_t servo_control_press(uint32_t press_ms);
esp_err_t servo_control_press_mode(servo_control_press_mode_t mode);
esp_err_t servo_control_press_custom(int angle, uint32_t press_ms);
esp_err_t servo_control_get_config(servo_control_config_t *config);
esp_err_t servo_control_update_config(const servo_control_config_t *config, bool persist, bool move_to_rest);
int servo_control_get_angle(void);
