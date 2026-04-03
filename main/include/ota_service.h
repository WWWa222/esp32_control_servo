#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    bool in_progress;
    bool rollback_pending_verify;
    bool last_success;
    int last_error;
    char status[32];
    char detail[96];
    char url[192];
} ota_service_status_t;

esp_err_t ota_service_init(void);
esp_err_t ota_service_mark_running_app_valid_if_needed(void);
esp_err_t ota_service_start(const char *url);
void ota_service_get_status(ota_service_status_t *status);
