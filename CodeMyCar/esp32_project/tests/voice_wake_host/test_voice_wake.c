#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "voice_wake.h"
#include "esp_wn_iface.h"
#include "model_path.h"
static model_iface_data_t instance;
static srmodel_list_t models={1};
static int detect_count, base, destroyed, deinit;
static bool missing, trigger;
static int rate=16000;
static model_iface_data_t *create(const void *n,det_mode_t m)
{assert(strcmp(n,"wn9s_nihaoxiaozhi")==0 && m==DET_MODE_90);return &instance;}
static int sample_rate(model_iface_data_t*p){(void)p;return rate;}
static int channels(model_iface_data_t*p){(void)p;return 1;}
static int chunk(model_iface_data_t*p){(void)p;return 512;}
static void clean(model_iface_data_t*p){(void)p;assert(!"SDK clean must not be called on C6");}
static void destroy(model_iface_data_t*p){(void)p;destroyed++;}
static wakenet_state_t detect(model_iface_data_t*p,int16_t *samples)
{(void)p;for(int i=0;i<512;i++)assert(samples[i]==base+i);base+=512;detect_count++;
 return trigger?WAKENET_DETECTED:WAKENET_NO_DETECT;}
static const esp_wn_iface_t engine={create,sample_rate,channels,chunk,detect,clean,destroy};
const esp_wn_iface_t *esp_wn_handle_from_name(const char*n){(void)n;return &engine;}
srmodel_list_t *esp_srmodel_init(const char*p){assert(!strcmp(p,"model"));return &models;}
char *esp_srmodel_filter(srmodel_list_t*p,const char*a,const char*b)
{assert(p==&models&&!strcmp(a,"wn9s_")&&!strcmp(b,"nihaoxiaozhi"));
 return missing?NULL:"wn9s_nihaoxiaozhi";}
void esp_srmodel_deinit(srmodel_list_t*p){assert(p==&models);deinit++;}
size_t esp_get_free_heap_size(void){return 200000;}
size_t heap_caps_get_largest_free_block(int p){(void)p;return 100000;}
int64_t esp_timer_get_time(void){static int64_t t;t+=100;return t;}
static void fill(int16_t*p,int start){for(int i=0;i<320;i++)p[i]=start+i;}
int main(void){
 int16_t pcm[320];
 assert(voice_wake_init());
 fill(pcm,0);assert(!voice_wake_feed(pcm,320,0));assert(detect_count==0);
 fill(pcm,320);assert(!voice_wake_feed(pcm,320,1));assert(detect_count==1);
 int old_destroyed=destroyed;
 base=1000;fill(pcm,1000);assert(!voice_wake_feed(pcm,320,5));assert(destroyed>old_destroyed);
 trigger=true;fill(pcm,1320);assert(voice_wake_feed(pcm,320,6));assert(detect_count==2);
 trigger=false;base=2000;fill(pcm,2000);assert(!voice_wake_feed(pcm,320,7));
 fill(pcm,2320);assert(!voice_wake_feed(pcm,320,8));assert(detect_count==3);
 voice_wake_destroy();assert(destroyed==3&&deinit==1);
 missing=true;assert(!voice_wake_init());assert(deinit==2);
 missing=false;models.num=2;assert(!voice_wake_init());assert(deinit==3);
 models.num=1;rate=8000;assert(!voice_wake_init());assert(destroyed==4&&deinit==4);
 puts("voice_wake: production adapter reframing, gap reset, trigger reset, format and model failures passed (mock engine)");
}
