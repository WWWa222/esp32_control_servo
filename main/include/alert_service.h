#pragma once

#include "esp_err.h"

esp_err_t alert_service_send_webhook(const char *event_name, const char *detail);
