#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "connect_wifi.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "cJSON.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "mqtt_client.h"
#include "connect_wifi.h"
static const char *TAG = "mqttwss_example";
#define CONFIG_BROKER_URI "mqtt://test.mosquitto.org"

#define TOPIC "oyvindrankelarsen/feeds/testfeed"
// https://wokwi.com/projects/377761117596697601

#define LED1_PIN 2
#define LED2_PIN 4
#define LED3_PIN 13

#define HASH_LEN 32
#define FIRMWARE_VERSION 1.9
#define UPDATE_JSON_URL "https://github.com/oyvindrankelarsen/esp32ota/raw/main/bin/firmware.json"

#define THEPIN 2

// receive buffer
char rcv_buffer[200];
static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    // your_context_t *context = event->context;
    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_subscribe(client, TOPIC, 1);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        if (strncmp(event->data, "RIGHT", event->data_len) == 0)
        {
            gpio_set_level(LED1_PIN, 1);
            vTaskDelay(3000 / portTICK_PERIOD_MS);
            gpio_set_level(LED1_PIN, 0);
        }
        if (strncmp(event->data, "MIDDLE", event->data_len) == 0)
        {
            gpio_set_level(LED2_PIN, 1);
            vTaskDelay(3000 / portTICK_PERIOD_MS);
            gpio_set_level(LED2_PIN, 0);
        }
        if (strncmp(event->data, "LEFT", event->data_len) == 0)
        {
            gpio_set_level(LED3_PIN, 1);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            gpio_set_level(LED3_PIN, 0);
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    /* The argument passed to esp_mqtt_client_register_event can de accessed as handler_args*/
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);
    mqtt_event_handler_cb(event_data);
}

static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_BROKER_URI,
    };

    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    esp_mqtt_client_start(client);
}

// esp_http_client event handler
esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{

    switch (evt->event_id)
    {
    case HTTP_EVENT_REDIRECT:
    case HTTP_EVENT_ERROR:
        break;
    case HTTP_EVENT_ON_CONNECTED:
        break;
    case HTTP_EVENT_HEADER_SENT:
        break;
    case HTTP_EVENT_ON_HEADER:
        break;
    case HTTP_EVENT_ON_DATA:
        if (!esp_http_client_is_chunked_response(evt->client))
        {
            strncpy(rcv_buffer, (char *)evt->data, evt->data_len);
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        break;
    case HTTP_EVENT_DISCONNECTED:
        break;
    }
    return ESP_OK;
}

void check_update_task(void *pvParameter)
{
    int cnt = 0;
    while (1)
    {
        char buf[255];
        sprintf(buf, "%s?token=%d", UPDATE_JSON_URL, cnt);
        cnt++;
        printf("Looking for a new firmware at %s", buf);

        // configure the esp_http_client
        esp_http_client_config_t config = {
            .url = buf,
            .event_handler = _http_event_handler,
            .keep_alive_enable = true,
            .timeout_ms = 30000,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);

        // downloading the json file
        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK)
        {

            // parse the json file
            cJSON *json = cJSON_Parse(rcv_buffer);
            if (json == NULL)
                printf("downloaded file is not a valid json, aborting...\n");
            else
            {
                cJSON *version = cJSON_GetObjectItemCaseSensitive(json, "version");
                cJSON *file = cJSON_GetObjectItemCaseSensitive(json, "file");

                // check the version
                if (!cJSON_IsNumber(version))
                    printf("unable to read new version, aborting...\n");
                else
                {

                    double new_version = version->valuedouble;
                    if (new_version > FIRMWARE_VERSION)
                    {

                        printf("current firmware version (%.1f) is lower than the available one (%.1f), upgrading...\n", FIRMWARE_VERSION, new_version);
                        if (cJSON_IsString(file) && (file->valuestring != NULL))
                        {
                            printf("downloading and installing new firmware (%s)...\n", file->valuestring);

                            esp_http_client_config_t ota_client_config = {
                                .url = file->valuestring,
                                .keep_alive_enable = true,
                            };
                            esp_https_ota_config_t ota_config = {
                                .http_config = &ota_client_config,
                            };
                            esp_err_t ret = esp_https_ota(&ota_config);
                            if (ret == ESP_OK)
                            {
                                printf("OTA OK, restarting...\n");
                                esp_restart();
                            }
                            else
                            {
                                printf("OTA failed...\n");
                            }
                        }
                        else
                            printf("unable to read the new file name, aborting...\n");
                    }
                    else
                        printf("current firmware version (%.1f) is greater or equal to the available one (%.1f), nothing to do...\n", FIRMWARE_VERSION, new_version);
                }
            }
        }
        else
            printf("unable to download the json file, aborting...\n");

        // cleanup
        esp_http_client_cleanup(client);

        printf("\n");
        vTaskDelay(30000 / portTICK_PERIOD_MS);
    }
}

static void print_sha256(const uint8_t *image_hash, const char *label)
{
    char hash_print[HASH_LEN * 2 + 1];
    hash_print[HASH_LEN * 2] = 0;
    for (int i = 0; i < HASH_LEN; ++i)
    {
        sprintf(&hash_print[i * 2], "%02x", image_hash[i]);
    }
    ESP_LOGI("SHA", "%s %s", label, hash_print);
}

static void get_sha256_of_partitions(void)
{
    uint8_t sha_256[HASH_LEN] = {0};
    esp_partition_t partition;

    // get sha256 digest for bootloader
    partition.address = ESP_BOOTLOADER_OFFSET;
    partition.size = ESP_PARTITION_TABLE_OFFSET;
    partition.type = ESP_PARTITION_TYPE_APP;
    esp_partition_get_sha256(&partition, sha_256);
    print_sha256(sha_256, "SHA-256 for bootloader: ");

    // get sha256 digest for running partition
    esp_partition_get_sha256(esp_ota_get_running_partition(), sha_256);
    print_sha256(sha_256, "SHA-256 for current firmware: ");
}

void app_main(void)
{
    gpio_reset_pin(LED1_PIN);
    gpio_set_direction(LED1_PIN, GPIO_MODE_OUTPUT);
    gpio_reset_pin(LED2_PIN);
    gpio_set_direction(LED2_PIN, GPIO_MODE_OUTPUT);
    gpio_reset_pin(LED3_PIN);
    gpio_set_direction(LED3_PIN, GPIO_MODE_OUTPUT);
    esp_err_t ret = nvs_flash_init();
    ESP_LOGI("CH", "%d ret", ret);
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    get_sha256_of_partitions();

    connect_wifi();
    esp_event_loop_create_default();
    mqtt_app_start();

    xTaskCreate(check_update_task, "check_update_task", 8192, NULL, 5, NULL);

    while (1)
    {
        gpio_set_level(THEPIN, 1);
        vTaskDelay(1500 / portTICK_PERIOD_MS);
        gpio_set_level(LED1_PIN, 0);
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}