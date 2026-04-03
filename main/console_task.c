#include "console_task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_service.h"
#include "sdkconfig.h"
#include "servo_control.h"
#include "status_store.h"
#include "wifi_service.h"

static const char *TAG = "console_task";

static void print_ota_status(void)
{
    ota_service_status_t status;
    ota_service_get_status(&status);
    ESP_LOGI(TAG,
             "ota status=%s in_progress=%s pending_verify=%s last_success=%s err=%d url=%s detail=%s",
             status.status,
             status.in_progress ? "yes" : "no",
             status.rollback_pending_verify ? "yes" : "no",
             status.last_success ? "yes" : "no",
             status.last_error,
             status.url,
             status.detail);
}

static char *skip_spaces(char *text)
{
    while (text != NULL && *text == ' ') {
        text++;
    }
    return text;
}

static void print_servo_config(void)
{
    servo_control_config_t config;
    if (servo_control_get_config(&config) != ESP_OK) {
        ESP_LOGE(TAG, "Unable to read servo config");
        return;
    }

    ESP_LOGI(TAG,
             "servo config min=%d max=%d rest=%d ready=%d short_angle=%d long_angle=%d short_ms=%lu long_ms=%lu prepare_ms=%lu release_ms=%lu",
             config.min_angle,
             config.max_angle,
             config.rest_angle,
             config.ready_angle,
             config.short_press_angle,
             config.long_press_angle,
             (unsigned long)config.short_press_ms,
             (unsigned long)config.long_press_ms,
             (unsigned long)config.prepare_settle_ms,
             (unsigned long)config.release_settle_ms);
}

static void print_wifi_profiles(void)
{
    wifi_service_profiles_snapshot_t snapshot;
    if (wifi_service_get_profiles_snapshot(&snapshot) != ESP_OK) {
        ESP_LOGE(TAG, "Unable to read Wi-Fi profile state");
        return;
    }

    for (size_t index = 0; index < snapshot.count; index++) {
        const wifi_service_profile_info_t *info = &snapshot.profiles[index];
        if (info->slot == WIFI_SERVICE_FALLBACK_SLOT) {
            ESP_LOGI(TAG,
                     "wifi fallback configured=%s active=%s ssid=%s",
                     info->configured ? "yes" : "no",
                     info->active ? "yes" : "no",
                     info->configured ? info->ssid : "-");
        } else {
            ESP_LOGI(TAG,
                     "wifi slot=%d configured=%s active=%s ssid=%s",
                     info->slot + 1,
                     info->configured ? "yes" : "no",
                     info->active ? "yes" : "no",
                     info->configured ? info->ssid : "-");
        }
    }
}

static void print_help(void)
{
    ESP_LOGI(TAG, "Commands:");
    ESP_LOGI(TAG, "  help");
    ESP_LOGI(TAG, "  status");
    ESP_LOGI(TAG, "  servo <angle>");
    ESP_LOGI(TAG, "  servo status");
    ESP_LOGI(TAG, "  servo config <rest|ready|short_angle|long_angle|short_ms|long_ms|prepare_ms|release_ms> <value>");
    ESP_LOGI(TAG, "  press short");
    ESP_LOGI(TAG, "  press long");
    ESP_LOGI(TAG, "  press <ms>");
    ESP_LOGI(TAG, "  press custom <angle> <ms>");
    ESP_LOGI(TAG, "  wifi status");
    ESP_LOGI(TAG, "  wifi list");
    ESP_LOGI(TAG, "  wifi set <slot> <ssid> [password]");
    ESP_LOGI(TAG, "  wifi ap");
    ESP_LOGI(TAG, "  wifi sta");
    ESP_LOGI(TAG, "  wifi clear");
    ESP_LOGI(TAG, "  wifi clear <slot>");
    ESP_LOGI(TAG, "  ota status");
    ESP_LOGI(TAG, "  ota start <https-url>");
    ESP_LOGI(TAG, "  heartbeat");
    ESP_LOGI(TAG, "  reboot");
}

static void print_status(void)
{
    app_status_snapshot_t snapshot;
    wifi_service_credentials_t credentials;
    const bool fallback_ap_active = wifi_service_is_fallback_ap_active();

    status_store_snapshot(&snapshot);
    memset(&credentials, 0, sizeof(credentials));
    (void)wifi_service_get_credentials(&credentials);

    ESP_LOGI(TAG,
             "status wifi=%s ip=%s ssid=%s source=%s active_slot=%d setup_ap=%s servo=%d host_online=%s host=%s user=%s hb_age_ms=%lld cpu=%.1f mem=%.1f press_count=%lu last_alert=%s",
             snapshot.wifi_connected ? "up" : "down",
             snapshot.ip_address,
             credentials.has_credentials ? credentials.ssid : "",
             credentials.loaded_from_nvs ? "nvs" : "sdkconfig",
             credentials.active_slot,
             fallback_ap_active ? "on" : "off",
             snapshot.servo_angle,
             snapshot.host_online ? "yes" : "no",
             snapshot.host_name,
             snapshot.user_name,
             snapshot.last_heartbeat_age_ms,
             (double)snapshot.cpu_pct,
             (double)snapshot.memory_pct,
             (unsigned long)snapshot.press_count,
             snapshot.last_alert_reason);
}

static void finalize_press_result(uint32_t press_ms)
{
    status_store_set_servo_angle(servo_control_get_angle());
    status_store_record_press(press_ms);
}

static void handle_press_command(char *argument)
{
    if (argument == NULL || argument[0] == '\0' || strcmp(argument, "short") == 0) {
        servo_control_config_t config;
        ESP_ERROR_CHECK(servo_control_get_config(&config));
        ESP_ERROR_CHECK(servo_control_press_mode(SERVO_CONTROL_PRESS_MODE_SHORT));
        finalize_press_result(config.short_press_ms);
        return;
    }

    if (strcmp(argument, "long") == 0) {
        servo_control_config_t config;
        ESP_ERROR_CHECK(servo_control_get_config(&config));
        ESP_ERROR_CHECK(servo_control_press_mode(SERVO_CONTROL_PRESS_MODE_LONG));
        finalize_press_result(config.long_press_ms);
        return;
    }

    if (strncmp(argument, "custom ", 7) == 0) {
        char *payload = argument + 7;
        char *saveptr = NULL;
        char *angle_text = strtok_r(payload, " ", &saveptr);
        char *ms_text = strtok_r(NULL, " ", &saveptr);
        if (angle_text == NULL || ms_text == NULL) {
            ESP_LOGW(TAG, "Usage: press custom <angle> <ms>");
            return;
        }

        const int angle = (int)strtol(angle_text, NULL, 10);
        const uint32_t press_ms = (uint32_t)strtoul(ms_text, NULL, 10);
        ESP_ERROR_CHECK(servo_control_press_custom(angle, press_ms));
        finalize_press_result(press_ms);
        return;
    }

    servo_control_config_t config;
    ESP_ERROR_CHECK(servo_control_get_config(&config));

    const uint32_t press_ms = (uint32_t)strtoul(argument, NULL, 10);
    ESP_ERROR_CHECK(servo_control_press_custom(config.short_press_angle, press_ms));
    finalize_press_result(press_ms);
}

static void handle_servo_command(char *argument)
{
    if (argument == NULL || argument[0] == '\0' || strcmp(argument, "status") == 0) {
        print_servo_config();
        return;
    }

    if (strncmp(argument, "config ", 7) == 0) {
        char *payload = argument + 7;
        char *saveptr = NULL;
        char *key = strtok_r(payload, " ", &saveptr);
        char *value_text = strtok_r(NULL, " ", &saveptr);

        if (key == NULL || value_text == NULL) {
            ESP_LOGW(TAG, "Usage: servo config <key> <value>");
            return;
        }

        servo_control_config_t config;
        ESP_ERROR_CHECK(servo_control_get_config(&config));

        const long value = strtol(value_text, NULL, 10);
        if (strcmp(key, "rest") == 0) {
            config.rest_angle = (int)value;
        } else if (strcmp(key, "ready") == 0) {
            config.ready_angle = (int)value;
        } else if (strcmp(key, "short_angle") == 0) {
            config.short_press_angle = (int)value;
        } else if (strcmp(key, "long_angle") == 0) {
            config.long_press_angle = (int)value;
        } else if (strcmp(key, "short_ms") == 0) {
            config.short_press_ms = (uint32_t)value;
        } else if (strcmp(key, "long_ms") == 0) {
            config.long_press_ms = (uint32_t)value;
        } else if (strcmp(key, "prepare_ms") == 0) {
            config.prepare_settle_ms = (uint32_t)value;
        } else if (strcmp(key, "release_ms") == 0) {
            config.release_settle_ms = (uint32_t)value;
        } else {
            ESP_LOGW(TAG, "Unknown servo config key: %s", key);
            return;
        }

        esp_err_t err = servo_control_update_config(&config, true, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Servo config update failed: %s", esp_err_to_name(err));
            return;
        }

        status_store_set_servo_angle(servo_control_get_angle());
        print_servo_config();
        return;
    }

    ESP_ERROR_CHECK(servo_control_set_angle((int)strtol(argument, NULL, 10)));
    status_store_set_servo_angle(servo_control_get_angle());
}

static void handle_wifi_command(char *arguments)
{
    if (arguments == NULL || arguments[0] == '\0' || strcmp(arguments, "status") == 0 ||
        strcmp(arguments, "list") == 0) {
        print_status();
        print_wifi_profiles();
        return;
    }

    char *saveptr = NULL;
    char *subcommand = strtok_r(arguments, " ", &saveptr);
    if (subcommand == NULL) {
        print_status();
        return;
    }

    if (strcmp(subcommand, "set") == 0) {
        char *slot_text = strtok_r(NULL, " ", &saveptr);
        char *ssid = strtok_r(NULL, " ", &saveptr);
        char *password = skip_spaces(saveptr);

        if (slot_text == NULL || ssid == NULL) {
            ESP_LOGW(TAG, "Usage: wifi set <slot> <ssid> [password]");
            return;
        }

        const int slot = (int)strtol(slot_text, NULL, 10) - 1;
        if (slot < 0 || slot >= WIFI_SERVICE_MAX_PROFILES) {
            ESP_LOGW(TAG, "Wi-Fi slot must be between 1 and %d", WIFI_SERVICE_MAX_PROFILES);
            return;
        }

        wifi_service_profile_t profiles[WIFI_SERVICE_MAX_PROFILES];
        esp_err_t err = wifi_service_get_saved_profiles(profiles, WIFI_SERVICE_MAX_PROFILES);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Unable to load Wi-Fi profiles: %s", esp_err_to_name(err));
            return;
        }

        strlcpy(profiles[slot].ssid, ssid, sizeof(profiles[slot].ssid));
        strlcpy(profiles[slot].password,
                (password != NULL && password[0] != '\0') ? password : "",
                sizeof(profiles[slot].password));

        err = wifi_service_set_profiles(profiles, WIFI_SERVICE_MAX_PROFILES);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi update failed: %s", esp_err_to_name(err));
            return;
        }

        ESP_LOGI(TAG, "Wi-Fi slot %d updated for SSID=%s", slot + 1, ssid);
        print_wifi_profiles();
        return;
    }

    if (strcmp(subcommand, "ap") == 0) {
        esp_err_t err = wifi_service_force_fallback_ap();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Fallback setup hotspot enable failed: %s", esp_err_to_name(err));
            return;
        }

        ESP_LOGI(TAG, "Fallback setup hotspot enabled");
        print_status();
        return;
    }

    if (strcmp(subcommand, "sta") == 0) {
        esp_err_t err = wifi_service_force_station_retry();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Station retry failed: %s", esp_err_to_name(err));
            return;
        }

        ESP_LOGI(TAG, "Station retry started");
        print_status();
        return;
    }

    if (strcmp(subcommand, "clear") == 0) {
        char *slot_text = strtok_r(NULL, " ", &saveptr);
        if (slot_text == NULL || strcmp(slot_text, "all") == 0) {
            esp_err_t err = wifi_service_clear_credentials();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Wi-Fi clear failed: %s", esp_err_to_name(err));
                return;
            }

            ESP_LOGI(TAG, "Stored runtime Wi-Fi profiles cleared");
            print_wifi_profiles();
            return;
        }

        const int slot = (int)strtol(slot_text, NULL, 10) - 1;
        if (slot < 0 || slot >= WIFI_SERVICE_MAX_PROFILES) {
            ESP_LOGW(TAG, "Wi-Fi slot must be between 1 and %d", WIFI_SERVICE_MAX_PROFILES);
            return;
        }

        wifi_service_profile_t profiles[WIFI_SERVICE_MAX_PROFILES];
        esp_err_t err = wifi_service_get_saved_profiles(profiles, WIFI_SERVICE_MAX_PROFILES);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Unable to load Wi-Fi profiles: %s", esp_err_to_name(err));
            return;
        }

        memset(&profiles[slot], 0, sizeof(profiles[slot]));
        err = wifi_service_set_profiles(profiles, WIFI_SERVICE_MAX_PROFILES);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi clear failed: %s", esp_err_to_name(err));
            return;
        }

        ESP_LOGI(TAG, "Wi-Fi slot %d cleared", slot + 1);
        print_wifi_profiles();
        return;
    }

    ESP_LOGW(TAG, "Unknown wifi command: %s", subcommand);
    ESP_LOGI(TAG, "Available wifi commands: status, list, set, clear");
}

static void console_task(void *arg)
{
    char line[192];

    print_help();
    while (true) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }

        char *saveptr = NULL;
        char *command = strtok_r(line, " ", &saveptr);
        char *argument = skip_spaces(saveptr);

        if (strcmp(command, "help") == 0) {
            print_help();
        } else if (strcmp(command, "status") == 0) {
            print_status();
        } else if (strcmp(command, "servo") == 0) {
            handle_servo_command(argument);
        } else if (strcmp(command, "press") == 0) {
            handle_press_command(argument);
        } else if (strcmp(command, "wifi") == 0) {
            handle_wifi_command(argument);
        } else if (strcmp(command, "ota") == 0) {
            if (argument == NULL || argument[0] == '\0' || strcmp(argument, "status") == 0) {
                print_ota_status();
            } else if (strncmp(argument, "start ", 6) == 0) {
                esp_err_t err = ota_service_start(argument + 6);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "OTA start failed: %s", esp_err_to_name(err));
                } else {
                    print_ota_status();
                }
            } else {
                ESP_LOGW(TAG, "Usage: ota status | ota start <https-url>");
            }
        } else if (strcmp(command, "heartbeat") == 0) {
            status_store_record_heartbeat("console", "manual", 0.0f, 0.0f, 0);
            print_status();
        } else if (strcmp(command, "reboot") == 0) {
            esp_restart();
        } else {
            ESP_LOGW(TAG, "Unknown command: %s", command);
            print_help();
        }
    }
}

void console_task_start(void)
{
    xTaskCreate(console_task, "console_task", 6144, NULL, 5, NULL);
}
