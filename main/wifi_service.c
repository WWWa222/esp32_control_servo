#include "wifi_service.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "status_store.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_NVS_NAMESPACE "rpc_wifi"
#define WIFI_NVS_PROFILES_KEY "profiles"
#define WIFI_NVS_SSID_KEY "ssid"
#define WIFI_NVS_PASSWORD_KEY "password"
#define WIFI_CONNECT_ATTEMPTS_PER_PROFILE 3

static const char *TAG = "wifi_service";

static EventGroupHandle_t s_wifi_event_group;
static SemaphoreHandle_t s_wifi_lock;
static esp_netif_t *s_wifi_sta_netif;
static esp_netif_t *s_wifi_ap_netif;
static esp_event_handler_instance_t s_wifi_event_handler_instance;
static esp_event_handler_instance_t s_ip_event_handler_instance;
static bool s_wifi_initialized;
static bool s_wifi_driver_initialized;
static bool s_wifi_started;
static bool s_wifi_connected;
static bool s_fallback_ap_active;
static bool s_wifi_auto_reconnect_enabled;
static bool s_wifi_reconfiguring;
static wifi_service_profile_t s_runtime_profiles[WIFI_SERVICE_MAX_PROFILES];
static int s_candidate_slots[WIFI_SERVICE_MAX_PROFILE_INFOS];
static size_t s_candidate_count;
static size_t s_current_candidate_index;
static int s_current_slot = WIFI_SERVICE_NO_SLOT;
static int s_active_slot = WIFI_SERVICE_NO_SLOT;
static int s_disconnects_for_current_candidate;
static bool s_current_loaded_from_nvs;

#if CONFIG_RPC_WIFI_USE_STATIC_IP
typedef struct {
    esp_netif_ip_info_t ip_info;
    esp_netif_dns_info_t dns_main;
    esp_netif_dns_info_t dns_backup;
    bool dns_main_configured;
    bool dns_backup_configured;
} wifi_static_ip_config_t;
#endif

static esp_err_t ensure_wifi_driver_initialized(void);
static esp_err_t configure_station_ip_settings(void);
static esp_err_t stop_wifi_station(void);
static esp_err_t start_candidate_for_slot(int slot);
static esp_err_t start_fallback_setup_ap(bool keep_station_enabled);

static const char *wifi_addressing_mode_string(void)
{
#if CONFIG_RPC_WIFI_USE_STATIC_IP
    return "static";
#else
    return "dhcp";
#endif
}

static bool fallback_setup_ap_supported(void)
{
#if CONFIG_RPC_WIFI_FALLBACK_AP_ENABLE
    return CONFIG_RPC_WIFI_FALLBACK_AP_SSID[0] != '\0';
#else
    return false;
#endif
}

#if CONFIG_RPC_WIFI_USE_STATIC_IP
static esp_err_t parse_ipv4_setting(const char *setting_name, const char *value, esp_ip4_addr_t *out_ip)
{
    if (value == NULL || value[0] == '\0') {
        ESP_LOGE(TAG, "Wi-Fi %s is empty", setting_name);
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t err = esp_netif_str_to_ip4(value, out_ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi %s is invalid: %s", setting_name, value);
        return err;
    }

    return ESP_OK;
}

static esp_err_t parse_optional_ipv4_setting(const char *setting_name,
                                             const char *value,
                                             esp_ip4_addr_t *out_ip,
                                             bool *configured)
{
    if (configured == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *configured = false;
    if (value == NULL || value[0] == '\0') {
        return ESP_OK;
    }

    const esp_err_t err = parse_ipv4_setting(setting_name, value, out_ip);
    if (err != ESP_OK) {
        return err;
    }

    *configured = true;
    return ESP_OK;
}

static esp_err_t load_static_ip_config(wifi_static_ip_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));

    esp_err_t err = parse_ipv4_setting("static IP", CONFIG_RPC_WIFI_STATIC_IP, &config->ip_info.ip);
    if (err != ESP_OK) {
        return err;
    }

    err = parse_ipv4_setting("static netmask", CONFIG_RPC_WIFI_STATIC_NETMASK, &config->ip_info.netmask);
    if (err != ESP_OK) {
        return err;
    }

    err = parse_ipv4_setting("static gateway", CONFIG_RPC_WIFI_STATIC_GATEWAY, &config->ip_info.gw);
    if (err != ESP_OK) {
        return err;
    }

    err = parse_optional_ipv4_setting("static DNS1",
                                      CONFIG_RPC_WIFI_STATIC_DNS1,
                                      &config->dns_main.ip.u_addr.ip4,
                                      &config->dns_main_configured);
    if (err != ESP_OK) {
        return err;
    }

    err = parse_optional_ipv4_setting("static DNS2",
                                      CONFIG_RPC_WIFI_STATIC_DNS2,
                                      &config->dns_backup.ip.u_addr.ip4,
                                      &config->dns_backup_configured);
    if (err != ESP_OK) {
        return err;
    }

    config->dns_main.ip.type = ESP_IPADDR_TYPE_V4;
    config->dns_backup.ip.type = ESP_IPADDR_TYPE_V4;
    return ESP_OK;
}
#endif

static bool wifi_profile_valid(const wifi_service_profile_t *profile)
{
    return profile != NULL && profile->ssid[0] != '\0';
}

static void clear_profile(wifi_service_profile_t *profile)
{
    if (profile != NULL) {
        memset(profile, 0, sizeof(*profile));
    }
}

static void set_wifi_connected_state(bool connected)
{
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    s_wifi_connected = connected;
    xSemaphoreGive(s_wifi_lock);
}

static void set_auto_reconnect_enabled(bool enabled)
{
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    s_wifi_auto_reconnect_enabled = enabled;
    xSemaphoreGive(s_wifi_lock);
}

static bool is_auto_reconnect_enabled(void)
{
    bool enabled;

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    enabled = s_wifi_auto_reconnect_enabled;
    xSemaphoreGive(s_wifi_lock);

    return enabled;
}

static bool is_wifi_reconfiguring(void)
{
    bool reconfiguring;

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    reconfiguring = s_wifi_reconfiguring;
    xSemaphoreGive(s_wifi_lock);

    return reconfiguring;
}

static void fill_fallback_profile(wifi_service_profile_t *profile)
{
    clear_profile(profile);
    strlcpy(profile->ssid, CONFIG_RPC_WIFI_SSID, sizeof(profile->ssid));
    strlcpy(profile->password, CONFIG_RPC_WIFI_PASSWORD, sizeof(profile->password));
}

static bool slot_to_profile_locked(int slot, wifi_service_profile_t *profile, bool *loaded_from_nvs)
{
    if (profile == NULL || loaded_from_nvs == NULL) {
        return false;
    }

    if (slot == WIFI_SERVICE_FALLBACK_SLOT) {
        fill_fallback_profile(profile);
        *loaded_from_nvs = false;
        return wifi_profile_valid(profile);
    }

    if (slot < 0 || slot >= WIFI_SERVICE_MAX_PROFILES) {
        return false;
    }

    *profile = s_runtime_profiles[slot];
    *loaded_from_nvs = true;
    return wifi_profile_valid(profile);
}

static int find_candidate_index_locked(int slot)
{
    for (size_t index = 0; index < s_candidate_count; index++) {
        if (s_candidate_slots[index] == slot) {
            return (int)index;
        }
    }
    return -1;
}

static void rebuild_candidates_locked(void)
{
    wifi_service_profile_t fallback_profile;
    fill_fallback_profile(&fallback_profile);

    s_candidate_count = 0;
    for (int slot = 0; slot < WIFI_SERVICE_MAX_PROFILES; slot++) {
        if (wifi_profile_valid(&s_runtime_profiles[slot])) {
            s_candidate_slots[s_candidate_count++] = slot;
        }
    }

    if (wifi_profile_valid(&fallback_profile)) {
        bool duplicate = false;
        for (size_t index = 0; index < s_candidate_count; index++) {
            const int slot = s_candidate_slots[index];
            if (slot >= 0 && strcmp(s_runtime_profiles[slot].ssid, fallback_profile.ssid) == 0) {
                duplicate = true;
                break;
            }
        }

        if (!duplicate) {
            s_candidate_slots[s_candidate_count++] = WIFI_SERVICE_FALLBACK_SLOT;
        }
    }

    if (s_candidate_count == 0) {
        s_current_candidate_index = 0;
        s_current_slot = WIFI_SERVICE_NO_SLOT;
        s_active_slot = WIFI_SERVICE_NO_SLOT;
        s_current_loaded_from_nvs = false;
        s_disconnects_for_current_candidate = 0;
        return;
    }

    int candidate_index = find_candidate_index_locked(s_current_slot);
    if (candidate_index < 0) {
        s_current_candidate_index = 0;
        s_current_slot = s_candidate_slots[0];
        s_current_loaded_from_nvs = (s_current_slot != WIFI_SERVICE_FALLBACK_SLOT);
        s_disconnects_for_current_candidate = 0;
    } else {
        s_current_candidate_index = (size_t)candidate_index;
    }

    if (s_active_slot != WIFI_SERVICE_NO_SLOT && find_candidate_index_locked(s_active_slot) < 0) {
        s_active_slot = WIFI_SERVICE_NO_SLOT;
    }
}

static esp_err_t load_legacy_profile_from_nvs(wifi_service_profile_t *profile)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }

    clear_profile(profile);

    size_t ssid_len = sizeof(profile->ssid);
    err = nvs_get_str(handle, WIFI_NVS_SSID_KEY, profile->ssid, &ssid_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    size_t password_len = sizeof(profile->password);
    err = nvs_get_str(handle, WIFI_NVS_PASSWORD_KEY, profile->password, &password_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        profile->password[0] = '\0';
        err = ESP_OK;
    }

    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    return wifi_profile_valid(profile) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t load_profiles_from_nvs(wifi_service_profile_t *profiles)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        clear_profile(&profiles[0]);
        return load_legacy_profile_from_nvs(&profiles[0]);
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t blob_len = sizeof(wifi_service_profile_t) * WIFI_SERVICE_MAX_PROFILES;
    err = nvs_get_blob(handle, WIFI_NVS_PROFILES_KEY, profiles, &blob_len);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        clear_profile(&profiles[0]);
        return load_legacy_profile_from_nvs(&profiles[0]);
    }
    if (err != ESP_OK) {
        return err;
    }
    if (blob_len != sizeof(wifi_service_profile_t) * WIFI_SERVICE_MAX_PROFILES) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t save_profiles_to_nvs(const wifi_service_profile_t *profiles)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle,
                       WIFI_NVS_PROFILES_KEY,
                       profiles,
                       sizeof(wifi_service_profile_t) * WIFI_SERVICE_MAX_PROFILES);
    if (err == ESP_OK) {
        (void)nvs_erase_key(handle, WIFI_NVS_SSID_KEY);
        (void)nvs_erase_key(handle, WIFI_NVS_PASSWORD_KEY);
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static esp_err_t clear_profiles_in_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    (void)nvs_erase_key(handle, WIFI_NVS_PROFILES_KEY);
    (void)nvs_erase_key(handle, WIFI_NVS_SSID_KEY);
    (void)nvs_erase_key(handle, WIFI_NVS_PASSWORD_KEY);
    err = nvs_commit(handle);

    nvs_close(handle);
    return err;
}

static esp_err_t refresh_cached_profiles(void)
{
    wifi_service_profile_t profiles[WIFI_SERVICE_MAX_PROFILES];
    memset(profiles, 0, sizeof(profiles));

    esp_err_t err = load_profiles_from_nvs(profiles);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        return err;
    }

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    memcpy(s_runtime_profiles, profiles, sizeof(s_runtime_profiles));
    rebuild_candidates_locked();
    xSemaphoreGive(s_wifi_lock);

    return ESP_OK;
}

static bool get_candidate_by_index(size_t candidate_index,
                                   int *slot,
                                   bool *loaded_from_nvs,
                                   wifi_service_profile_t *profile)
{
    bool found = false;

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    if (candidate_index < s_candidate_count) {
        *slot = s_candidate_slots[candidate_index];
        found = slot_to_profile_locked(*slot, profile, loaded_from_nvs);
    }
    xSemaphoreGive(s_wifi_lock);

    return found;
}

static esp_err_t start_candidate_by_index(size_t candidate_index)
{
    int slot = WIFI_SERVICE_NO_SLOT;
    bool loaded_from_nvs = false;
    bool fallback_ap_active = false;
    wifi_service_profile_t profile;
    clear_profile(&profile);

    if (!get_candidate_by_index(candidate_index, &slot, &loaded_from_nvs, &profile)) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = ensure_wifi_driver_initialized();
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .failure_retry_cnt = 1,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };
    strlcpy((char *)wifi_config.sta.ssid, profile.ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, profile.password, sizeof(wifi_config.sta.password));

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    s_current_candidate_index = candidate_index;
    s_current_slot = slot;
    s_current_loaded_from_nvs = loaded_from_nvs;
    s_disconnects_for_current_candidate = 0;
    fallback_ap_active = s_fallback_ap_active;
    s_wifi_reconfiguring = true;
    xSemaphoreGive(s_wifi_lock);

    set_auto_reconnect_enabled(false);
    set_wifi_connected_state(false);
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    status_store_set_wifi(false, "");

    if (s_wifi_started) {
        err = esp_wifi_disconnect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT && err != ESP_ERR_WIFI_MODE) {
            xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
            s_wifi_reconfiguring = false;
            xSemaphoreGive(s_wifi_lock);
            return err;
        }
    }

    if (fallback_ap_active) {
        err = start_fallback_setup_ap(true);
        if (err != ESP_OK) {
            xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
            s_wifi_reconfiguring = false;
            xSemaphoreGive(s_wifi_lock);
            return err;
        }
    }

    err = configure_station_ip_settings();
    if (err != ESP_OK) {
        xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
        s_wifi_reconfiguring = false;
        xSemaphoreGive(s_wifi_lock);
        return err;
    }

    err = esp_wifi_set_mode(fallback_ap_active ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    }
    if (err == ESP_OK && !s_wifi_started) {
        err = esp_wifi_start();
        if (err == ESP_OK) {
            s_wifi_started = true;
        }
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_ps(WIFI_PS_NONE);
    }

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    s_wifi_reconfiguring = false;
    xSemaphoreGive(s_wifi_lock);

    if (err != ESP_OK) {
        return err;
    }

    set_auto_reconnect_enabled(true);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG,
             "Wi-Fi station started for SSID=%s (%s, %s)",
             profile.ssid,
             loaded_from_nvs ? "nvs" : "sdkconfig",
             wifi_addressing_mode_string());
    return ESP_OK;
}

static esp_err_t start_candidate_for_slot(int slot)
{
    int candidate_index = -1;

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    candidate_index = find_candidate_index_locked(slot);
    xSemaphoreGive(s_wifi_lock);

    if (candidate_index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    return start_candidate_by_index((size_t)candidate_index);
}

static esp_err_t start_next_candidate(void)
{
    size_t next_candidate_index;

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    if (s_candidate_count == 0) {
        xSemaphoreGive(s_wifi_lock);
        return ESP_ERR_NOT_FOUND;
    }
    next_candidate_index = (s_current_candidate_index + 1) % s_candidate_count;
    xSemaphoreGive(s_wifi_lock);

    return start_candidate_by_index(next_candidate_index);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)event_data;
        const int disconnect_reason = event != NULL ? (int)event->reason : 0;

        set_wifi_connected_state(false);
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        status_store_set_wifi(false, "");

        if (is_wifi_reconfiguring()) {
            ESP_LOGI(TAG, "Ignoring Wi-Fi disconnect during reconfigure, reason=%d", disconnect_reason);
            return;
        }

        if (disconnect_reason == WIFI_REASON_ASSOC_LEAVE) {
            ESP_LOGI(TAG, "Ignoring intentional Wi-Fi disconnect");
            return;
        }

        if (!is_auto_reconnect_enabled()) {
            ESP_LOGI(TAG, "Wi-Fi disconnected and auto reconnect is disabled");
            return;
        }

        size_t candidate_count = 0;
        size_t current_candidate_index = 0;
        int failure_count = 0;

        xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
        s_active_slot = WIFI_SERVICE_NO_SLOT;
        s_disconnects_for_current_candidate++;
        candidate_count = s_candidate_count;
        current_candidate_index = s_current_candidate_index;
        failure_count = s_disconnects_for_current_candidate;
        xSemaphoreGive(s_wifi_lock);

        if (candidate_count > 1 && failure_count >= WIFI_CONNECT_ATTEMPTS_PER_PROFILE) {
            const size_t next_candidate_index = (current_candidate_index + 1) % candidate_count;
            if (next_candidate_index == 0 && fallback_setup_ap_supported()) {
                esp_err_t ap_err = start_fallback_setup_ap(true);
                if (ap_err != ESP_OK) {
                    ESP_LOGW(TAG, "Unable to enable fallback setup hotspot: %s", esp_err_to_name(ap_err));
                }
            }

            ESP_LOGW(TAG,
                     "Wi-Fi candidate %u failed %d times (reason=%d), switching to the next profile",
                     (unsigned)current_candidate_index,
                     failure_count,
                     disconnect_reason);
            if (start_next_candidate() == ESP_OK) {
                return;
            }
        }

        if (candidate_count == 1 && failure_count >= WIFI_CONNECT_ATTEMPTS_PER_PROFILE && fallback_setup_ap_supported()) {
            esp_err_t ap_err = start_fallback_setup_ap(true);
            if (ap_err != ESP_OK) {
                ESP_LOGW(TAG, "Unable to enable fallback setup hotspot: %s", esp_err_to_name(ap_err));
            }
        }

        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying current profile (reason=%d)", disconnect_reason);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        char ip_string[16];
        char netmask_string[16];
        char gateway_string[16];

        esp_ip4addr_ntoa(&event->ip_info.ip, ip_string, sizeof(ip_string));
        esp_ip4addr_ntoa(&event->ip_info.netmask, netmask_string, sizeof(netmask_string));
        esp_ip4addr_ntoa(&event->ip_info.gw, gateway_string, sizeof(gateway_string));
        set_wifi_connected_state(true);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        status_store_set_wifi(true, ip_string);

        xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
        s_active_slot = s_current_slot;
        s_disconnects_for_current_candidate = 0;
        xSemaphoreGive(s_wifi_lock);

        ESP_LOGI(TAG,
                 "Wi-Fi connected, IP=%s netmask=%s gateway=%s mode=%s",
                 ip_string,
                 netmask_string,
                 gateway_string,
                 wifi_addressing_mode_string());
    }
}

static esp_err_t init_nvs_flash(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t ensure_wifi_driver_initialized(void)
{
    if (s_wifi_driver_initialized) {
        return ESP_OK;
    }

    s_wifi_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_wifi_sta_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_wifi_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_wifi_ap_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              &wifi_event_handler,
                                              NULL,
                                              &s_wifi_event_handler_instance);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_STA_GOT_IP,
                                              &wifi_event_handler,
                                              NULL,
                                              &s_ip_event_handler_instance);
    if (err != ESP_OK) {
        return err;
    }

    s_wifi_driver_initialized = true;
    return ESP_OK;
}

static esp_err_t configure_station_ip_settings(void)
{
    if (s_wifi_sta_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_RPC_WIFI_USE_STATIC_IP
    wifi_static_ip_config_t static_config;
    char ip_string[16];
    char netmask_string[16];
    char gateway_string[16];
    char dns1_string[16] = "-";
    char dns2_string[16] = "-";

    esp_err_t err = load_static_ip_config(&static_config);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_dhcpc_stop(s_wifi_sta_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return err;
    }

    err = esp_netif_set_ip_info(s_wifi_sta_netif, &static_config.ip_info);
    if (err != ESP_OK) {
        return err;
    }

    if (static_config.dns_main_configured) {
        err = esp_netif_set_dns_info(s_wifi_sta_netif, ESP_NETIF_DNS_MAIN, &static_config.dns_main);
        if (err != ESP_OK) {
            return err;
        }
        esp_ip4addr_ntoa(&static_config.dns_main.ip.u_addr.ip4, dns1_string, sizeof(dns1_string));
    }

    if (static_config.dns_backup_configured) {
        err = esp_netif_set_dns_info(s_wifi_sta_netif, ESP_NETIF_DNS_BACKUP, &static_config.dns_backup);
        if (err != ESP_OK) {
            return err;
        }
        esp_ip4addr_ntoa(&static_config.dns_backup.ip.u_addr.ip4, dns2_string, sizeof(dns2_string));
    }

    esp_ip4addr_ntoa(&static_config.ip_info.ip, ip_string, sizeof(ip_string));
    esp_ip4addr_ntoa(&static_config.ip_info.netmask, netmask_string, sizeof(netmask_string));
    esp_ip4addr_ntoa(&static_config.ip_info.gw, gateway_string, sizeof(gateway_string));

    ESP_LOGI(TAG,
             "Wi-Fi static IPv4 configured ip=%s netmask=%s gateway=%s dns1=%s dns2=%s",
             ip_string,
             netmask_string,
             gateway_string,
             dns1_string,
             dns2_string);
    return ESP_OK;
#else
    const esp_err_t err = esp_netif_dhcpc_start(s_wifi_sta_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        return err;
    }

    ESP_LOGI(TAG, "Wi-Fi DHCP client enabled");
    return ESP_OK;
#endif
}

static esp_err_t start_fallback_setup_ap(bool keep_station_enabled)
{
#if !CONFIG_RPC_WIFI_FALLBACK_AP_ENABLE
    (void)keep_station_enabled;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!fallback_setup_ap_supported()) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t password_len = strlen(CONFIG_RPC_WIFI_FALLBACK_AP_PASSWORD);
    if (password_len > WIFI_SERVICE_PASSWORD_MAX_LEN || (password_len > 0 && password_len < 8)) {
        ESP_LOGE(TAG, "Fallback setup hotspot password must be empty or at least 8 characters");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_wifi_driver_initialized();
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = strlen(CONFIG_RPC_WIFI_FALLBACK_AP_SSID),
            .channel = CONFIG_RPC_WIFI_FALLBACK_AP_CHANNEL,
            .max_connection = 4,
        },
    };
    strlcpy((char *)ap_config.ap.ssid, CONFIG_RPC_WIFI_FALLBACK_AP_SSID, sizeof(ap_config.ap.ssid));

    if (password_len == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        strlcpy((char *)ap_config.ap.password, CONFIG_RPC_WIFI_FALLBACK_AP_PASSWORD, sizeof(ap_config.ap.password));
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    err = esp_wifi_set_mode(keep_station_enabled ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    }
    if (err == ESP_OK && !s_wifi_started) {
        err = esp_wifi_start();
        if (err == ESP_OK) {
            s_wifi_started = true;
        }
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_ps(WIFI_PS_NONE);
    }
    if (err != ESP_OK) {
        return err;
    }

    bool already_active = false;
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    already_active = s_fallback_ap_active;
    s_fallback_ap_active = true;
    xSemaphoreGive(s_wifi_lock);

    if (!already_active) {
        ESP_LOGW(TAG,
                 "Fallback setup hotspot enabled SSID=%s IP=%s mode=%s",
                 CONFIG_RPC_WIFI_FALLBACK_AP_SSID,
                 WIFI_SERVICE_FALLBACK_AP_IP,
                 keep_station_enabled ? "apsta" : "ap");
    }

    return ESP_OK;
#endif
}

static esp_err_t stop_wifi_station(void)
{
    if (!s_wifi_driver_initialized) {
        xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
        s_fallback_ap_active = false;
        xSemaphoreGive(s_wifi_lock);
        set_auto_reconnect_enabled(false);
        set_wifi_connected_state(false);
        status_store_set_wifi(false, "");
        return ESP_OK;
    }

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    s_wifi_reconfiguring = true;
    xSemaphoreGive(s_wifi_lock);

    set_auto_reconnect_enabled(false);

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_INIT &&
        err != ESP_ERR_WIFI_NOT_CONNECT && err != ESP_ERR_WIFI_MODE) {
        xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
        s_wifi_reconfiguring = false;
        xSemaphoreGive(s_wifi_lock);
        return err;
    }

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
        s_wifi_reconfiguring = false;
        xSemaphoreGive(s_wifi_lock);
        return err;
    }

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    s_wifi_reconfiguring = false;
    s_wifi_started = false;
    s_active_slot = WIFI_SERVICE_NO_SLOT;
    s_fallback_ap_active = false;
    xSemaphoreGive(s_wifi_lock);

    set_wifi_connected_state(false);
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    status_store_set_wifi(false, "");
    return ESP_OK;
}

bool wifi_service_has_credentials(void)
{
    wifi_service_credentials_t credentials;
    if (wifi_service_get_credentials(&credentials) != ESP_OK) {
        return false;
    }
    return credentials.has_credentials;
}

bool wifi_service_is_connected(void)
{
    bool connected;

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    connected = s_wifi_connected;
    xSemaphoreGive(s_wifi_lock);

    return connected;
}

bool wifi_service_is_fallback_ap_active(void)
{
    bool active;

    if (s_wifi_lock == NULL) {
        return false;
    }

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    active = s_fallback_ap_active;
    xSemaphoreGive(s_wifi_lock);

    return active;
}

esp_err_t wifi_service_force_fallback_ap(void)
{
    if (!s_wifi_initialized) {
        esp_err_t err = wifi_service_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    if (!fallback_setup_ap_supported()) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    s_active_slot = WIFI_SERVICE_NO_SLOT;
    xSemaphoreGive(s_wifi_lock);

    set_auto_reconnect_enabled(false);
    set_wifi_connected_state(false);
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    status_store_set_wifi(false, "");
    return start_fallback_setup_ap(true);
}

esp_err_t wifi_service_force_station_retry(void)
{
    if (!s_wifi_initialized) {
        esp_err_t err = wifi_service_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    esp_err_t err = refresh_cached_profiles();
    if (err != ESP_OK) {
        return err;
    }

    size_t candidate_count = 0;
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    candidate_count = s_candidate_count;
    xSemaphoreGive(s_wifi_lock);

    if (candidate_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    return start_candidate_by_index(0);
}

esp_err_t wifi_service_init(void)
{
    if (s_wifi_initialized) {
        return ESP_OK;
    }

    esp_err_t err = init_nvs_flash();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_wifi_lock = xSemaphoreCreateMutex();
    if (s_wifi_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = refresh_cached_profiles();
    if (err != ESP_OK) {
        return err;
    }

    s_wifi_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_service_start(void)
{
    if (!s_wifi_initialized) {
        esp_err_t err = wifi_service_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    esp_err_t err = refresh_cached_profiles();
    if (err != ESP_OK) {
        return err;
    }

    size_t candidate_count = 0;
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    candidate_count = s_candidate_count;
    xSemaphoreGive(s_wifi_lock);

    if (candidate_count == 0) {
        set_auto_reconnect_enabled(false);
        if (fallback_setup_ap_supported()) {
            ESP_LOGW(TAG, "Wi-Fi credentials are empty; enabling fallback setup hotspot");
            return start_fallback_setup_ap(false);
        }

        ESP_LOGW(TAG, "Wi-Fi credentials are empty; skipping station startup");
        return ESP_ERR_INVALID_STATE;
    }

    return start_candidate_by_index(0);
}

esp_err_t wifi_service_update_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t ssid_len = strlen(ssid);
    const size_t password_len = password != NULL ? strlen(password) : 0;
    if (ssid_len == 0 || ssid_len > WIFI_SERVICE_SSID_MAX_LEN ||
        password_len > WIFI_SERVICE_PASSWORD_MAX_LEN ||
        (password_len > 0 && password_len < 8)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_wifi_initialized) {
        esp_err_t err = wifi_service_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    wifi_service_profile_t profiles[WIFI_SERVICE_MAX_PROFILES];
    memset(profiles, 0, sizeof(profiles));
    esp_err_t err = wifi_service_get_saved_profiles(profiles, WIFI_SERVICE_MAX_PROFILES);
    if (err != ESP_OK) {
        return err;
    }
    strlcpy(profiles[0].ssid, ssid, sizeof(profiles[0].ssid));
    strlcpy(profiles[0].password, password != NULL ? password : "", sizeof(profiles[0].password));

    return wifi_service_set_profiles(profiles, WIFI_SERVICE_MAX_PROFILES);
}

esp_err_t wifi_service_set_profiles(const wifi_service_profile_t *profiles, size_t count)
{
    if (profiles == NULL || count > WIFI_SERVICE_MAX_PROFILES) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_service_profile_t normalized_profiles[WIFI_SERVICE_MAX_PROFILES];
    wifi_service_profile_t previous_profiles[WIFI_SERVICE_MAX_PROFILES];
    int preferred_slot = WIFI_SERVICE_NO_SLOT;
    memset(normalized_profiles, 0, sizeof(normalized_profiles));
    memset(previous_profiles, 0, sizeof(previous_profiles));

    if (!s_wifi_initialized) {
        esp_err_t err = wifi_service_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    memcpy(previous_profiles, s_runtime_profiles, sizeof(previous_profiles));
    xSemaphoreGive(s_wifi_lock);

    for (size_t index = 0; index < count; index++) {
        const size_t ssid_len = strlen(profiles[index].ssid);
        const size_t password_len = strlen(profiles[index].password);
        if (ssid_len > WIFI_SERVICE_SSID_MAX_LEN || password_len > WIFI_SERVICE_PASSWORD_MAX_LEN ||
            (password_len > 0 && password_len < 8)) {
            return ESP_ERR_INVALID_ARG;
        }

        if (ssid_len == 0) {
            continue;
        }

        strlcpy(normalized_profiles[index].ssid, profiles[index].ssid, sizeof(normalized_profiles[index].ssid));
        strlcpy(normalized_profiles[index].password,
                profiles[index].password,
                sizeof(normalized_profiles[index].password));

        if (preferred_slot == WIFI_SERVICE_NO_SLOT &&
            (!wifi_profile_valid(&previous_profiles[index]) ||
             strcmp(previous_profiles[index].ssid, normalized_profiles[index].ssid) != 0 ||
             strcmp(previous_profiles[index].password, normalized_profiles[index].password) != 0)) {
            preferred_slot = (int)index;
        }
    }

    esp_err_t err = save_profiles_to_nvs(normalized_profiles);
    if (err != ESP_OK) {
        return err;
    }

    err = refresh_cached_profiles();
    if (err != ESP_OK) {
        return err;
    }

    size_t candidate_count = 0;
    bool connected = false;
    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    candidate_count = s_candidate_count;
    connected = s_wifi_connected;
    xSemaphoreGive(s_wifi_lock);

    if (candidate_count > 0) {
        if (preferred_slot == WIFI_SERVICE_NO_SLOT && connected) {
            ESP_LOGI(TAG, "Wi-Fi profiles unchanged, keeping current connection");
            return ESP_OK;
        }

        if (preferred_slot != WIFI_SERVICE_NO_SLOT) {
            err = start_candidate_for_slot(preferred_slot);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Wi-Fi preferred slot %d selected after profile update", preferred_slot + 1);
                return ESP_OK;
            }

            ESP_LOGW(TAG,
                     "Wi-Fi preferred slot %d unavailable after update, falling back to first candidate",
                     preferred_slot + 1);
        }

        return start_candidate_by_index(0);
    }

    set_auto_reconnect_enabled(false);
    if (fallback_setup_ap_supported()) {
        return start_fallback_setup_ap(false);
    }

    return stop_wifi_station();
}

esp_err_t wifi_service_clear_credentials(void)
{
    if (!s_wifi_initialized) {
        esp_err_t err = wifi_service_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    esp_err_t err = clear_profiles_in_nvs();
    if (err != ESP_OK) {
        return err;
    }

    err = refresh_cached_profiles();
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    const size_t candidate_count = s_candidate_count;
    xSemaphoreGive(s_wifi_lock);

    if (candidate_count > 0) {
        return start_candidate_by_index(0);
    }

    set_auto_reconnect_enabled(false);
    if (fallback_setup_ap_supported()) {
        return start_fallback_setup_ap(false);
    }

    return stop_wifi_station();
}

esp_err_t wifi_service_get_credentials(wifi_service_credentials_t *credentials)
{
    if (credentials == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_wifi_initialized) {
        esp_err_t err = wifi_service_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    wifi_service_profile_t runtime_profiles[WIFI_SERVICE_MAX_PROFILES];
    int slot = WIFI_SERVICE_NO_SLOT;
    bool loaded_from_nvs = false;
    size_t candidate_count = 0;
    int candidate_slots[WIFI_SERVICE_MAX_PROFILE_INFOS];

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    memcpy(runtime_profiles, s_runtime_profiles, sizeof(runtime_profiles));
    slot = (s_current_slot != WIFI_SERVICE_NO_SLOT) ? s_current_slot :
           ((s_candidate_count > 0) ? s_candidate_slots[0] : WIFI_SERVICE_NO_SLOT);
    loaded_from_nvs = s_current_loaded_from_nvs;
    candidate_count = s_candidate_count;
    memcpy(candidate_slots, s_candidate_slots, sizeof(candidate_slots));
    xSemaphoreGive(s_wifi_lock);

    memset(credentials, 0, sizeof(*credentials));
    credentials->active_slot = slot;

    if (candidate_count == 0 && slot == WIFI_SERVICE_NO_SLOT) {
        return ESP_OK;
    }

    wifi_service_profile_t profile;
    clear_profile(&profile);

    if (slot == WIFI_SERVICE_FALLBACK_SLOT) {
        fill_fallback_profile(&profile);
        loaded_from_nvs = false;
    } else if (slot >= 0 && slot < WIFI_SERVICE_MAX_PROFILES) {
        profile = runtime_profiles[slot];
        loaded_from_nvs = true;
    } else if (candidate_count > 0) {
        const int first_slot = candidate_slots[0];
        if (first_slot == WIFI_SERVICE_FALLBACK_SLOT) {
            fill_fallback_profile(&profile);
            credentials->active_slot = WIFI_SERVICE_FALLBACK_SLOT;
            loaded_from_nvs = false;
        } else if (first_slot >= 0 && first_slot < WIFI_SERVICE_MAX_PROFILES) {
            profile = runtime_profiles[first_slot];
            credentials->active_slot = first_slot;
            loaded_from_nvs = true;
        }
    }

    if (!wifi_profile_valid(&profile)) {
        return ESP_OK;
    }

    credentials->has_credentials = true;
    credentials->loaded_from_nvs = loaded_from_nvs;
    strlcpy(credentials->ssid, profile.ssid, sizeof(credentials->ssid));
    strlcpy(credentials->password, profile.password, sizeof(credentials->password));
    return ESP_OK;
}

esp_err_t wifi_service_get_saved_profiles(wifi_service_profile_t *profiles, size_t count)
{
    if (profiles == NULL || count < WIFI_SERVICE_MAX_PROFILES) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_wifi_initialized) {
        esp_err_t err = wifi_service_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    memcpy(profiles, s_runtime_profiles, sizeof(s_runtime_profiles));
    xSemaphoreGive(s_wifi_lock);
    return ESP_OK;
}

esp_err_t wifi_service_get_profiles_snapshot(wifi_service_profiles_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_wifi_initialized) {
        esp_err_t err = wifi_service_init();
        if (err != ESP_OK) {
            return err;
        }
    }

    wifi_service_profile_t runtime_profiles[WIFI_SERVICE_MAX_PROFILES];
    int effective_active_slot;

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    memcpy(runtime_profiles, s_runtime_profiles, sizeof(runtime_profiles));
    effective_active_slot = (s_active_slot != WIFI_SERVICE_NO_SLOT) ? s_active_slot : s_current_slot;
    xSemaphoreGive(s_wifi_lock);

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->active_slot = effective_active_slot;

    for (int slot = 0; slot < WIFI_SERVICE_MAX_PROFILES; slot++) {
        wifi_service_profile_info_t *info = &snapshot->profiles[snapshot->count++];
        info->slot = slot;
        info->configured = wifi_profile_valid(&runtime_profiles[slot]);
        info->active = (slot == effective_active_slot);
        info->loaded_from_nvs = true;
        strlcpy(info->ssid, runtime_profiles[slot].ssid, sizeof(info->ssid));
    }

    wifi_service_profile_t fallback_profile;
    fill_fallback_profile(&fallback_profile);
    if (wifi_profile_valid(&fallback_profile) && snapshot->count < WIFI_SERVICE_MAX_PROFILE_INFOS) {
        wifi_service_profile_info_t *info = &snapshot->profiles[snapshot->count++];
        info->slot = WIFI_SERVICE_FALLBACK_SLOT;
        info->configured = true;
        info->active = (WIFI_SERVICE_FALLBACK_SLOT == effective_active_slot);
        info->loaded_from_nvs = false;
        strlcpy(info->ssid, fallback_profile.ssid, sizeof(info->ssid));
    }

    return ESP_OK;
}
