#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define ESP_ERR_NO_MEM 4
#define ESP_ERR_INVALID_ARG 5
#define CMCAR_ERR_MQTT_PUB 6
#define CMCAR_ERR_MQTT_SUB 7
#define CONFIG_CMCAR_WIFI_RECONNECT_BASE_MS 1000
#ifndef CONFIG_CMCAR_WIFI_MAX_RETRY
#define CONFIG_CMCAR_WIFI_MAX_RETRY 0
#endif
#define CONFIG_CMCAR_WIFI_SSID "test"
#define CONFIG_CMCAR_WIFI_PASSWORD "test"
#define CONFIG_CMCAR_MQTT_BROKER_URL "mqtt://test"
#define CONFIG_CMCAR_MQTT_CLIENT_ID "test"
#define CONFIG_CMCAR_MQTT_USERNAME ""
#define CONFIG_CMCAR_MQTT_PASSWORD ""
#define CONFIG_CMCAR_MQTT_KEEPALIVE_SEC 30
#define CONFIG_CMCAR_MQTT_BUFFER_SIZE 1024
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_ERROR_CHECK(x) ((void)(x))
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY UINT32_MAX
typedef unsigned TickType_t;
typedef struct {bool held;} FakeMutex;
typedef FakeMutex* SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t,unsigned);
int xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
typedef struct FakeTimer {void(*callback)(void*);void* arg;bool armed;uint64_t delay;} *esp_timer_handle_t;
typedef struct {void(*callback)(void*);void* arg;const char* name;} esp_timer_create_args_t;
int esp_timer_create(const esp_timer_create_args_t*,esp_timer_handle_t*);
int esp_timer_stop(esp_timer_handle_t);
int esp_timer_start_once(esp_timer_handle_t,uint64_t);
int esp_timer_delete(esp_timer_handle_t);
const char* esp_err_to_name(int);
typedef int esp_event_base_t;
#define ESP_EVENT_DECLARE_BASE(x) extern int x
#define ESP_EVENT_ANY_ID -1
#define WIFI_EVENT 1
#define IP_EVENT 2
#define IP_EVENT_STA_GOT_IP 1
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_CONNECTED 2
#define WIFI_EVENT_STA_DISCONNECTED 3
int esp_event_post(int,int,const void*,size_t,unsigned);
int esp_event_handler_instance_register(int,int,void(*)(void*,int,int32_t,void*),void*,void*);
#define WIFI_MODE_STA 0
#define WIFI_IF_STA 0
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
typedef struct {int unused;} wifi_init_config_t;
typedef struct {struct {char ssid[32];char password[64];struct{int authmode;}threshold;}sta;}wifi_config_t;
typedef struct {int reason;}wifi_event_sta_disconnected_t;
typedef struct {struct {struct{uint32_t addr;}ip;}ip_info;}ip_event_got_ip_t;
int esp_wifi_connect(void);
int esp_netif_init(void);
void* esp_netif_create_default_wifi_sta(void);
int esp_wifi_init(const wifi_init_config_t*);
int esp_wifi_set_mode(int);
int esp_wifi_set_config(int,const wifi_config_t*);
int esp_wifi_start(void);
int esp_wifi_stop(void);
typedef void* esp_mqtt_client_handle_t;
typedef struct {struct{struct{const char* uri;}address;}broker;struct{const char* client_id;const char* username;struct{const char* password;}authentication;}credentials;struct{int keepalive;bool disable_keepalive;}session;struct{int size;}buffer;}esp_mqtt_client_config_t;
typedef struct {int error_type;}fake_mqtt_error;
typedef struct FakeMqttEvent {int event_id;const char* topic;int topic_len;const char* data;int data_len;fake_mqtt_error* error_handle;}*esp_mqtt_event_handle_t;
#define MQTT_EVENT_CONNECTED 1
#define MQTT_EVENT_DISCONNECTED 2
#define MQTT_EVENT_DATA 3
#define MQTT_EVENT_ERROR 4
void* esp_mqtt_client_init(const esp_mqtt_client_config_t*);
int esp_mqtt_client_register_event(void*,int,void(*)(void*,int,int32_t,void*),void*);
int esp_mqtt_client_start(void*);
int esp_mqtt_client_stop(void*);
int esp_mqtt_client_destroy(void*);
int esp_mqtt_client_subscribe(void*,const char*,int);
int esp_mqtt_client_publish(void*,const char*,const char*,int,int,int);
