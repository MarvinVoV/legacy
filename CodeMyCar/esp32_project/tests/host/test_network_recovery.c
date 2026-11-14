/* Compile real production services with deterministic timer/driver failures. */
#include <assert.h>
#include <stdio.h>
#include "network_fake.h"
#ifdef TEST_WIFI
#include "../../components/wifi_service/src/wifi_service.c"
#else
#include "../../components/mqtt_service/src/mqtt_service.c"
#endif
int CMCAR_EVENTS;
static struct FakeTimer timer;
static FakeMutex mutex;
static int connect_calls,start_calls,stop_calls,posted_events;
static int connect_result=ESP_FAIL,start_result=ESP_FAIL,timer_create_result=ESP_OK;
SemaphoreHandle_t xSemaphoreCreateMutex(void){mutex.held=false;return &mutex;}
int xSemaphoreTake(SemaphoreHandle_t m,unsigned wait){(void)wait;if(m->held)return 0;m->held=true;return 1;}
int xSemaphoreGive(SemaphoreHandle_t m){assert(m->held);m->held=false;return 1;}
void vSemaphoreDelete(SemaphoreHandle_t m){(void)m;}
int esp_timer_create(const esp_timer_create_args_t*a,esp_timer_handle_t*t){if(timer_create_result!=ESP_OK)return timer_create_result;timer=(struct FakeTimer){.callback=a->callback,.arg=a->arg};*t=&timer;return ESP_OK;}
int esp_timer_stop(esp_timer_handle_t t){t->armed=false;return ESP_OK;}
int esp_timer_start_once(esp_timer_handle_t t,uint64_t delay){t->armed=true;t->delay=delay;return ESP_OK;}
int esp_timer_delete(esp_timer_handle_t t){t->armed=false;return ESP_OK;}
static void fire(void){assert(timer.armed);timer.armed=false;timer.callback(timer.arg);}
const char*esp_err_to_name(int n){(void)n;return "injected";}
int esp_event_post(int a,int b,const void*c,size_t d,unsigned e){(void)a;(void)b;(void)c;(void)d;(void)e;++posted_events;return ESP_OK;}
int esp_event_handler_instance_register(int a,int b,void(*c)(void*,int,int32_t,void*),void*d,void*e){(void)a;(void)b;(void)c;(void)d;(void)e;return ESP_OK;}
int esp_wifi_connect(void){++connect_calls;return connect_result;}
int esp_netif_init(void){return ESP_OK;}
void*esp_netif_create_default_wifi_sta(void){return &timer;}
int esp_wifi_init(const wifi_init_config_t*a){(void)a;return ESP_OK;}
int esp_wifi_set_mode(int a){(void)a;return ESP_OK;}
int esp_wifi_set_config(int a,const wifi_config_t*b){(void)a;(void)b;return ESP_OK;}
int esp_wifi_start(void){return ESP_OK;}
int esp_wifi_stop(void){return ESP_OK;}
void*esp_mqtt_client_init(const esp_mqtt_client_config_t*a){(void)a;return &mutex;}
int esp_mqtt_client_register_event(void*a,int b,void(*c)(void*,int,int32_t,void*),void*d){(void)a;(void)b;(void)c;(void)d;return ESP_OK;}
int esp_mqtt_client_start(void*a){(void)a;++start_calls;return start_result;}
int esp_mqtt_client_stop(void*a){(void)a;++stop_calls;return ESP_OK;}
int esp_mqtt_client_destroy(void*a){(void)a;return ESP_OK;}
int esp_mqtt_client_subscribe(void*a,const char*b,int c){(void)a;(void)b;(void)c;return 1;}
int esp_mqtt_client_publish(void*a,const char*b,const char*c,int d,int e,int f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 1;}
int main(void)
{
#ifdef TEST_WIFI
 assert(wifi_service_init()==ESP_OK);assert(wifi_service_start()==ESP_OK);
 /* No DISCONNECTED event: synchronous errors still re-arm with backoff. */
 wifi_event_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_START,NULL);
 assert(connect_calls==1 && timer.armed && timer.delay==1000000);
 fire();assert(connect_calls==2 && timer.armed && timer.delay==2000000);
 connect_result=ESP_OK;fire();assert(connect_calls==3 && !timer.armed);
 wifi_event_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_CONNECTED,NULL);
 assert(wifi_service_is_connected() && s_retry_count==0);
 /* Duplicate STA_START must not reconnect an associated client. */
 wifi_event_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_START,NULL);assert(connect_calls==3);
 wifi_event_sta_disconnected_t ev={.reason=1};
 wifi_event_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&ev);
 assert(!wifi_service_is_connected() && timer.armed && timer.delay==1000000);
 assert(wifi_service_stop()==ESP_OK);assert(!timer.armed);
 reconnect_timer_cb(NULL);wifi_event_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_START,NULL);
 assert(connect_calls==3); /* late callbacks cannot revive stopped service */
 unsigned posts=posted_events;
 ip_event_got_ip_t ip={0};
 wifi_event_handler(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);
 assert(posted_events==posts); /* stale IP must not revive MQTT either */
 assert(wifi_service_start()==ESP_OK);connect_result=ESP_FAIL;
 if (CONFIG_CMCAR_WIFI_MAX_RETRY == 0) {
  for(unsigned i=0;i<80;++i){retry_connect_later();assert(timer.armed && timer.delay<=60000000);}
  assert(s_retry_count==30);
 } else {
  retry_connect_later();
  unsigned before=connect_calls;
  while(timer.armed) fire();
  assert(connect_calls-before==CONFIG_CMCAR_WIFI_MAX_RETRY);
  assert(s_retry_count==CONFIG_CMCAR_WIFI_MAX_RETRY);
 }
 wifi_service_stop();
 puts("WiFi recovery: synchronous failure, backoff/cap, success, duplicate events, stop/late timer PASS");
#else
 assert(mqtt_service_init()==ESP_OK);
 assert(mqtt_service_start()==ESP_FAIL);assert(s_running && timer.armed && timer.delay==2000000);
 fire();assert(start_calls==2 && timer.armed);
 start_result=ESP_OK;fire();assert(start_calls==3 && s_started && !timer.armed);
 assert(mqtt_service_start()==ESP_OK && start_calls==3); /* new GOT_IP: idempotent */
 struct FakeMqttEvent event={.event_id=MQTT_EVENT_DISCONNECTED};
 mqtt_event_handler(NULL,0,0,&event);
 assert(mqtt_service_start()==ESP_OK && start_calls==3); /* SDK owns network reconnect */
 assert(mqtt_service_stop()==ESP_OK && stop_calls==1 && !s_started);
 startup_retry_cb(NULL);assert(start_calls==3);
 /* Busy publication mutex must defer, never block esp_timer or lose intent. */
 mutex.held=true;assert(mqtt_service_start()==ESP_ERR_TIMEOUT && timer.armed);
 mutex.held=false;fire();assert(start_calls==4 && s_started);
 mqtt_service_stop();start_result=ESP_FAIL;
 assert(mqtt_service_start()==ESP_FAIL && timer.armed);
 mqtt_service_stop();assert(!timer.armed);startup_retry_cb(NULL);assert(start_calls==5);
 event.event_id=MQTT_EVENT_CONNECTED;
 mqtt_event_handler(NULL,0,0,&event);assert(!mqtt_service_is_connected());
 mqtt_service_destroy();assert(s_client==NULL && s_start_retry_timer==NULL);
 assert(mqtt_service_start()==ESP_ERR_INVALID_STATE);
 timer_create_result=ESP_FAIL;assert(mqtt_service_init()==ESP_FAIL);
 assert(s_client==NULL && s_mutex==NULL); /* failed retry setup cleans ownership */
 puts("MQTT recovery: repeated startup failure, retry success, idempotence, SDK reconnect, busy mutex, stop/destroy PASS");
#endif
 return 0;
}
