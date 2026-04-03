#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define WIFI_SERVICE_SSID_MAX_LEN       32
#define WIFI_SERVICE_PASSWORD_MAX_LEN   64
#define WIFI_SERVICE_MAX_PROFILES       3
#define WIFI_SERVICE_FALLBACK_SLOT      (-1)
#define WIFI_SERVICE_NO_SLOT            (-2)
#define WIFI_SERVICE_MAX_PROFILE_INFOS  (WIFI_SERVICE_MAX_PROFILES + 1)
#define WIFI_SERVICE_FALLBACK_AP_IP     "192.168.4.1"

typedef struct {
    char ssid[WIFI_SERVICE_SSID_MAX_LEN + 1];
    char password[WIFI_SERVICE_PASSWORD_MAX_LEN + 1];
} wifi_service_profile_t;

typedef struct {
    bool has_credentials;
    bool loaded_from_nvs;
    int active_slot;
    char ssid[WIFI_SERVICE_SSID_MAX_LEN + 1];
    char password[WIFI_SERVICE_PASSWORD_MAX_LEN + 1];
} wifi_service_credentials_t;

typedef struct {
    int slot;
    bool configured;
    bool active;
    bool loaded_from_nvs;
    char ssid[WIFI_SERVICE_SSID_MAX_LEN + 1];
} wifi_service_profile_info_t;

typedef struct {
    size_t count;
    int active_slot;
    wifi_service_profile_info_t profiles[WIFI_SERVICE_MAX_PROFILE_INFOS];
} wifi_service_profiles_snapshot_t;

esp_err_t wifi_service_init(void);
esp_err_t wifi_service_start(void);
esp_err_t wifi_service_update_credentials(const char *ssid, const char *password);
esp_err_t wifi_service_set_profiles(const wifi_service_profile_t *profiles, size_t count);
esp_err_t wifi_service_clear_credentials(void);
esp_err_t wifi_service_get_credentials(wifi_service_credentials_t *credentials);
esp_err_t wifi_service_get_saved_profiles(wifi_service_profile_t *profiles, size_t count);
esp_err_t wifi_service_get_profiles_snapshot(wifi_service_profiles_snapshot_t *snapshot);
bool wifi_service_has_credentials(void);
bool wifi_service_is_connected(void);
bool wifi_service_is_fallback_ap_active(void);
esp_err_t wifi_service_force_fallback_ap(void);
esp_err_t wifi_service_force_station_retry(void);
