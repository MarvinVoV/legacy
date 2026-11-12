#pragma once
#include "host.h"
typedef void *esp_websocket_client_handle_t;
typedef const char *esp_event_base_t;
typedef void (*ws_callback_t)(void*,esp_event_base_t,int32_t,void*);
enum {WEBSOCKET_EVENT_ANY,WEBSOCKET_EVENT_CONNECTED,WEBSOCKET_EVENT_DISCONNECTED,
      WEBSOCKET_EVENT_ERROR,WEBSOCKET_EVENT_DATA};
typedef struct { const char *data_ptr; int data_len,op_code,payload_offset,payload_len; bool fin; } esp_websocket_event_data_t;
typedef struct { const char *uri; bool disable_auto_reconnect; int network_timeout_ms,buffer_size,task_stack,task_prio; } esp_websocket_client_config_t;
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t*);
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t,int,ws_callback_t,void*);
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t);
void esp_websocket_client_stop(esp_websocket_client_handle_t);
void esp_websocket_client_destroy(esp_websocket_client_handle_t);
int esp_websocket_client_send_text(esp_websocket_client_handle_t,const char*,int,unsigned);
int esp_websocket_client_send_bin(esp_websocket_client_handle_t,const char*,int,unsigned);
