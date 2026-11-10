#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "voice_frontend.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
static srmodel_list_t models = {3};
static esp_afe_sr_data_t instance;
static afe_config_t cfg;
static bool missing_models, missing_wake, timeout;
static int channels = 1, created, destroyed, deinit, fed;
static int enabled, disabled, ns_enabled, ns_disabled;
static int16_t output[512];
static afe_fetch_result_t result = {.data=output, .data_size=sizeof(output),
    .wakeup_state=WAKENET_DETECTED, .vad_state=VAD_SPEECH};
srmodel_list_t *esp_srmodel_init(const char *p)
{ assert(!strcmp(p,"model")); return missing_models ? NULL : &models; }
char *esp_srmodel_filter(srmodel_list_t *m, const char *prefix, const char *word)
{
 assert(m==&models);
 if (!strcmp(prefix,"wn9_")) { assert(!strcmp(word,"nihaoxiaozhi")); return missing_wake ? NULL : "wn9_nihaoxiaozhi_tts"; }
 if (!strcmp(prefix,"nsnet")) return "nsnet2";
 if (!strcmp(prefix,"vadnet")) return "vadnet1_medium";
 assert(false); return NULL;
}
void esp_srmodel_deinit(srmodel_list_t *m) { assert(m==&models); deinit++; }
afe_config_t *afe_config_init(const char *input, srmodel_list_t *m, int type, int mode)
{ assert(!strcmp(input,"M") && m==&models && type==AFE_TYPE_FD && mode==AFE_MODE_LOW_COST); memset(&cfg,0,sizeof(cfg)); return &cfg; }
void afe_config_free(afe_config_t *c) { assert(c==&cfg); }
static esp_afe_sr_data_t *create(afe_config_t *c)
{
 assert(!c->aec_init && !c->se_init && !c->agc_init);
 assert(c->wakenet_init && c->vad_init);
 assert(c->ns_init == (bool)CONFIG_CMCAR_VOICE_DENOISE);
 assert(c->memory_alloc_mode==AFE_MEMORY_ALLOC_MORE_PSRAM && c->afe_perferred_core==1);
 created++; return &instance;
}
static void destroy(esp_afe_sr_data_t *p) { assert(p==&instance); destroyed++; }
static int feed(esp_afe_sr_data_t *p, const int16_t *data)
{ assert(p==&instance); for(int i=0;i<512;i++) assert(data[i]==fed*512+i); fed++; return 512; }
static afe_fetch_result_t *fetch(esp_afe_sr_data_t *p, int ticks)
{ assert(p==&instance && ticks==100); return timeout ? NULL : &result; }
static int channel_count(esp_afe_sr_data_t *p) { (void)p; return channels; }
static int rate(esp_afe_sr_data_t *p) { (void)p; return 16000; }
static int chunk(esp_afe_sr_data_t *p) { (void)p; return 512; }
static int enable(esp_afe_sr_data_t *p) { (void)p; enabled++; return 1; }
static int disable(esp_afe_sr_data_t *p) { (void)p; disabled++; return 1; }
static int ns_on(esp_afe_sr_data_t *p) { (void)p; ns_enabled++; return 1; }
static int ns_off(esp_afe_sr_data_t *p) { (void)p; ns_disabled++; return 1; }
static void print(esp_afe_sr_data_t *p) { (void)p; }
static const esp_afe_sr_iface_t iface = {.create_from_config=create, .destroy=destroy,
 .feed=feed, .fetch_with_delay=fetch, .get_feed_channel_num=channel_count,
 .get_samp_rate=rate, .get_feed_chunksize=chunk, .enable_wakenet=enable,
 .disable_wakenet=disable, .enable_ns=ns_on, .disable_ns=ns_off, .print_pipeline=print};
const esp_afe_sr_iface_t *esp_afe_handle_from_config(afe_config_t *c) { (void)c; return &iface; }
void *heap_caps_malloc(size_t n, unsigned caps) { assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)); return malloc(n); }
void heap_caps_free(void *p) { free(p); }
int main(void)
{
 const int16_t *pcm; size_t samples; bool wake, speech;
 missing_models=true; assert(!voice_frontend_init()); assert(deinit==0);
 missing_models=false; missing_wake=true; assert(!voice_frontend_init()); assert(deinit==1);
 missing_wake=false; channels=2; assert(!voice_frontend_init()); assert(destroyed==1);
 channels=1; assert(voice_frontend_init()); assert(voice_frontend_init()); assert(created==2);
 int16_t input[1024]; for(int i=0;i<1024;i++) input[i]=i;
 assert(voice_frontend_feed(input,320)); assert(fed==0);
 assert(voice_frontend_feed(input+320,320)); assert(fed==1);
 assert(voice_frontend_feed(input+640,384)); assert(fed==2);
 assert(!voice_frontend_feed(NULL,10));
 assert(voice_frontend_fetch(&pcm,&samples,&wake,&speech,true)); assert(samples==512 && wake && speech);
 assert(voice_frontend_fetch(&pcm,&samples,&wake,&speech,false)); assert(!wake && disabled==1 && ns_enabled==CONFIG_CMCAR_VOICE_DENOISE);
 assert(voice_frontend_fetch(&pcm,&samples,&wake,&speech,false)); assert(disabled==1);
 assert(voice_frontend_fetch(&pcm,&samples,&wake,&speech,true)); assert(enabled==1 && ns_disabled==2*CONFIG_CMCAR_VOICE_DENOISE);
 timeout=true; assert(!voice_frontend_fetch(&pcm,&samples,&wake,&speech,true)); assert(!pcm && !samples && !wake);
 timeout=false; result.data_size=3; assert(!voice_frontend_fetch(&pcm,&samples,&wake,&speech,true));
 voice_frontend_destroy(); voice_frontend_destroy(); assert(destroyed==2);
 assert(!voice_frontend_feed(input,1));
 puts("S3 frontend: frame continuity, model failures, controls, half duplex and cleanup passed (mock AFE)");
}
