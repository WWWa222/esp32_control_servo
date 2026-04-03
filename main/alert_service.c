#include "alert_service.h"

#include "esp_log.h"
#include "status_store.h"

static const char *TAG = "alert_service";

esp_err_t alert_service_send_webhook(const char *event_name, const char *detail)
{
    const char *name = (event_name != NULL && event_name[0] != '\0') ? event_name : "alert";
    ESP_LOGW(TAG, "Alert event=%s detail=%s", name, detail ? detail : "-");
    status_store_record_alert(name);
    return ESP_OK;
}
