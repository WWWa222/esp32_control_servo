#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    bool enabled;
    bool configured;
    bool active;
    bool peer_up;
    bool time_synced;
    bool waiting_for_network;
    int endpoint_port;
    int keepalive_sec;
    char local_ip[48];
    char local_mask[48];
    char endpoint[96];
    char last_error[96];
} wireguard_service_status_t;

esp_err_t wireguard_service_init(void);
void wireguard_service_start(void);
esp_err_t wireguard_service_force_disconnect(void);
esp_err_t wireguard_service_force_reconnect(void);
void wireguard_service_get_status(wireguard_service_status_t *status);
bool wireguard_service_is_enabled(void);
