#define _POSIX_C_SOURCE 200809L
#include "host.h"
#include "voice_output.h"
#include "voice_dtd.h"
#include "voice_transport.h"
#include "esp_websocket_client.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

void *heap_caps_malloc(size_t n,unsigned caps) { (void)caps; return malloc(n); }
void heap_caps_free(void *p) { free(p); }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "fake"; }
int64_t esp_timer_get_time(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
void vTaskDelay(unsigned ticks) {
 assert(ticks>0); /* production must really block, even at the board's 100Hz */
 unsigned ms=ticks*HOST_TICK_MS;
 struct timespec t={.tv_sec=ms/1000,.tv_nsec=(long)(ms%1000)*1000000}; nanosleep(&t,NULL);
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { pthread_mutex_t *m=malloc(sizeof(*m)); pthread_mutex_init(m,NULL); return m; }
void xSemaphoreTake(SemaphoreHandle_t m,unsigned ticks) { (void)ticks; pthread_mutex_lock(m); }
void xSemaphoreGive(SemaphoreHandle_t m) { pthread_mutex_unlock(m); }
void vSemaphoreDelete(SemaphoreHandle_t m) { pthread_mutex_destroy(m); free(m); }
QueueHandle_t xQueueCreateStatic(unsigned n,unsigned item,uint8_t *storage,StaticQueue_t *q) {
 memset(q,0,sizeof(*q)); pthread_mutex_init(&q->mutex,NULL); pthread_cond_init(&q->readable,NULL);
 q->storage=storage;q->capacity=n;q->item_size=item;return q;
}
int xQueueReceive(QueueHandle_t q,void *p,unsigned wait) {
 pthread_mutex_lock(&q->mutex);
 while(!q->count) { if(!wait) {pthread_mutex_unlock(&q->mutex);return 0;} pthread_cond_wait(&q->readable,&q->mutex); }
 memcpy(p,q->storage+q->head*q->item_size,q->item_size);q->head=(q->head+1)%q->capacity;--q->count;
 pthread_mutex_unlock(&q->mutex);return pdTRUE;
}
int xQueueSend(QueueHandle_t q,const void *p,unsigned wait) {
 (void)wait;pthread_mutex_lock(&q->mutex);
 if(q->count==q->capacity) {pthread_mutex_unlock(&q->mutex);return 0;}
 memcpy(q->storage+((q->head+q->count)%q->capacity)*q->item_size,p,q->item_size);++q->count;
 pthread_cond_signal(&q->readable);pthread_mutex_unlock(&q->mutex);return pdTRUE;
}
void xQueueReset(QueueHandle_t q) {pthread_mutex_lock(&q->mutex);q->head=q->count=0;pthread_mutex_unlock(&q->mutex);}
void vQueueDelete(QueueHandle_t q) { (void)q; }
typedef struct {void (*fn)(void*);void *arg;} launch_t;
static void *launch(void *p) {launch_t l=*(launch_t*)p;free(p);l.fn(l.arg);return NULL;}
int xTaskCreate(void (*fn)(void*),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *task) {
 (void)name;(void)stack;(void)priority;*task=malloc(sizeof(**task));launch_t *l=malloc(sizeof(*l));*l=(launch_t){fn,arg};
 return pthread_create(*task,NULL,launch,l)==0 ? pdPASS:0;
}
void vTaskDelete(TaskHandle_t t) { pthread_cancel(*t);pthread_join(*t,NULL);free(t); }
static atomic_bool blocked,release_write,flushed;
static atomic_int writes;
bool voice_codec_has_output(const voice_codec_t *c) {return c->caps.output_channels>0;}
esp_err_t voice_codec_write(voice_codec_t *c,const int16_t *pcm,size_t n,int rate) {
 (void)c;assert(n>0 && rate==16000);
 if(pcm[0]==111) {atomic_store(&blocked,true);while(!atomic_load(&release_write)) vTaskDelay(1);}
 atomic_fetch_add(&writes,1);return ESP_OK;
}
static void *flush(void *p) { (void)p;voice_output_flush();atomic_store(&flushed,true);return NULL; }
static void wait_flag(atomic_bool *flag) {
 int64_t end=esp_timer_get_time()+2000000;
 while(!atomic_load(flag)) {assert(esp_timer_get_time()<end);vTaskDelay(1);}
}
EventGroupHandle_t xEventGroupCreate(void) {EventGroupHandle_t g=malloc(sizeof(*g));atomic_init(g,0);return g;}
unsigned xEventGroupGetBits(EventGroupHandle_t g) {return atomic_load(g);}
unsigned xEventGroupSetBits(EventGroupHandle_t g,unsigned bits) {return atomic_fetch_or(g,bits)|bits;}
unsigned xEventGroupClearBits(EventGroupHandle_t g,unsigned bits) {return atomic_fetch_and(g,~bits);}
unsigned xEventGroupWaitBits(EventGroupHandle_t g,unsigned b,int clear,int all,unsigned ticks) {
 (void)b;(void)clear;(void)all;(void)ticks;return atomic_load(g);
}
void vEventGroupDelete(EventGroupHandle_t g) {free(g);}
static ws_callback_t ws_callback;
static void *ws_arg;
static bool enable_barge=true;
static char last_sent[2048];
static void text_event(const char *text) {
 esp_websocket_event_data_t e={.data_ptr=text,.data_len=(int)strlen(text),.op_code=1,.payload_len=(int)strlen(text)};
 ws_callback(ws_arg,NULL,WEBSOCKET_EVENT_DATA,&e);
}
static void binary_event(const char *data,int n) {
 esp_websocket_event_data_t e={.data_ptr=data,.data_len=n,.op_code=2,.payload_len=n};
 ws_callback(ws_arg,NULL,WEBSOCKET_EVENT_DATA,&e);
}
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *c) {(void)c;return (void*)1;}
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t c,int event,ws_callback_t cb,void *arg) {
 (void)c;(void)event;ws_callback=cb;ws_arg=arg;return ESP_OK;
}
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t c) {(void)c;ws_callback(ws_arg,NULL,WEBSOCKET_EVENT_CONNECTED,NULL);return ESP_OK;}
void esp_websocket_client_stop(esp_websocket_client_handle_t c) {(void)c;}
void esp_websocket_client_destroy(esp_websocket_client_handle_t c) {(void)c;}
int esp_websocket_client_send_text(esp_websocket_client_handle_t c,const char *text,int n,unsigned ticks) {
 (void)c;(void)ticks;assert(n<(int)sizeof(last_sent));memcpy(last_sent,text,n);last_sent[n]=0;
 if(strstr(text,"\"type\":\"start\"")) text_event(enable_barge ?
   "{\"type\":\"ready\",\"barge_in\":true,\"playback_ack\":true}" :
   "{\"type\":\"ready\",\"barge_in\":false,\"playback_ack\":true}");
 return n;
}
int esp_websocket_client_send_bin(esp_websocket_client_handle_t c,const char *p,int n,unsigned ticks) {(void)c;(void)p;(void)ticks;return n;}
static esp_err_t receive_audio(const int16_t *pcm,size_t n,int rate,int channels,uint32_t epoch,void *arg) {
 (void)arg;return voice_output_write_epoch(pcm,n,rate,channels,epoch);
}
static void transport_test(void) {
 voice_transport_t *t=NULL;
 assert(voice_transport_open(&t,true,false,VOICE_SESSION_CONVERSATION)==ESP_OK);
 voice_transport_set_audio_callback(t,receive_audio,NULL);
 assert(voice_transport_barge_enabled(t));
 text_event("{\"type\":\"speak\",\"turn\":1,\"seq\":1,\"bytes\":640}");
 assert(voice_transport_barge_in(t)==ESP_OK);
 int before=atomic_load(&writes);
 text_event("{\"type\":\"speak\",\"turn\":1,\"seq\":2,\"bytes\":640}");
 char raw[640]={0};binary_event(raw,sizeof(raw));vTaskDelay(60);
 assert(atomic_load(&writes)==before && !voice_transport_is_speaking(t));
 text_event("{\"type\":\"stop\",\"turn\":1}");
 assert(voice_transport_poll(t)==ESP_OK && strstr(last_sent,"stop_ack"));
 text_event("{\"type\":\"speak\",\"turn\":2,\"seq\":3,\"bytes\":640}");
 uint32_t epoch=voice_output_epoch();
 text_event("{\"type\":\"stop\",\"turn\":1}");
 assert(voice_output_epoch()==epoch && voice_transport_is_speaking(t));
 binary_event(raw,1);binary_event(raw+1,639);
 text_event("{\"type\":\"playback_end\",\"turn\":2,\"seq\":3}");
 assert(voice_output_wait_drained(1000)==ESP_OK);
 assert(voice_transport_poll(t)==ESP_OK && strstr(last_sent,"playback_finished"));
 assert(!voice_transport_is_speaking(t));
 assert(!(voice_transport_events(t)&VOICE_PEER_FAILED));
 voice_transport_close(t);
 enable_barge=false;
 assert(voice_transport_open(&t,true,false,VOICE_SESSION_CONVERSATION)==ESP_OK);
 assert(!voice_transport_barge_enabled(t));
 assert(voice_transport_barge_in(t)==ESP_ERR_INVALID_STATE);
 voice_transport_close(t);
}
int main(void) {
 voice_dtd_t d={0};
 for(int i=0;i<400;i++) assert(!voice_dtd_update(&d,true,false,5000,320)); /* noise without speech */
 assert(!voice_dtd_update(&d,false,true,900,320));
 int triggered=0;for(int i=0;i<40;i++) triggered+=voice_dtd_update(&d,true,true,900,320);
 assert(triggered==1); /* 300ms warmup + 200ms fresh speech, then cooldown */
 assert(!voice_dtd_update(&d,true,true,900,0));
 voice_codec_t codec={.caps={.output_channels=2}};
 assert(voice_output_init(&codec)==ESP_OK);
 uint32_t old=voice_output_epoch();int16_t pcm[640];for(int i=0;i<640;i++) pcm[i]=111;
 assert(voice_output_write_epoch(pcm,640,16000,1,old)==ESP_OK);wait_flag(&blocked);
 pthread_t thread;pthread_create(&thread,NULL,flush,NULL);
 vTaskDelay(10);assert(!atomic_load(&flushed)); /* flush must fence an in-flight codec write */
 atomic_store(&release_write,true);pthread_join(thread,NULL);
 int completed=atomic_load(&writes);vTaskDelay(160);
 assert(atomic_load(&writes)==completed); /* no cancelled queued frame resumes */
 assert(!voice_output_active());
 assert(voice_output_write_epoch(pcm,320,16000,1,old)==ESP_ERR_INVALID_STATE);
 for(int i=0;i<640;i++) pcm[i]=222;
 assert(voice_output_write(pcm,320,16000,1)==ESP_OK);
 assert(voice_output_active());
 assert(voice_output_wait_drained(10)==ESP_ERR_TIMEOUT);
 assert(voice_output_wait_drained(1000)==ESP_OK);
 assert(atomic_load(&writes)==completed+1);
 transport_test();
 voice_output_destroy();
 puts("Playback epochs, in-flight cancellation fence, DMA tail guard, and fresh-window DTD passed");
}
