#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "model_path.h"
#include "esp_wn_iface.h"
typedef enum { AFE_MODE_LOW_COST, AFE_MODE_HIGH_PERF } afe_mode_t;
typedef enum { AEC_MODE_FD_LOW_COST, AEC_MODE_FD_HIGH_PERF } aec_mode_t;
enum { AFE_TYPE_FD, AEC_NLP_LEVEL_VERYAGGR, VAD_MODE_2, AFE_NS_MODE_NET,
       AFE_NS_MODE_WEBRTC, AFE_MEMORY_ALLOC_MORE_PSRAM, VAD_SPEECH };
typedef struct {
 bool aec_init, se_init, agc_init, vad_init, wakenet_init, ns_init;
 int vad_mode, vad_min_speech_ms, vad_min_noise_ms;
 int aec_mode, aec_filter_length, aec_nlp_level;
 char *vad_model_name, *wakenet_model_name, *wakenet_model_name_2, *ns_model_name;
 int wakenet_mode, afe_ns_mode, memory_alloc_mode, afe_perferred_core, afe_perferred_priority;
} afe_config_t;
typedef struct { int unused; } esp_afe_sr_data_t;
typedef struct { int16_t *data; int data_size, ret_value, wakeup_state, vad_state; } afe_fetch_result_t;
typedef struct {
 esp_afe_sr_data_t *(*create_from_config)(afe_config_t *);
 void (*destroy)(esp_afe_sr_data_t *);
 int (*feed)(esp_afe_sr_data_t *, const int16_t *);
 afe_fetch_result_t *(*fetch_with_delay)(esp_afe_sr_data_t *, int);
 int (*get_feed_channel_num)(esp_afe_sr_data_t *);
 int (*get_samp_rate)(esp_afe_sr_data_t *);
 int (*get_feed_chunksize)(esp_afe_sr_data_t *);
 int (*enable_wakenet)(esp_afe_sr_data_t *);
 int (*disable_wakenet)(esp_afe_sr_data_t *);
 int (*enable_ns)(esp_afe_sr_data_t *);
 int (*disable_ns)(esp_afe_sr_data_t *);
 void (*print_pipeline)(esp_afe_sr_data_t *);
} esp_afe_sr_iface_t;
afe_config_t *afe_config_init(const char *, srmodel_list_t *, int, int);
void afe_config_free(afe_config_t *);
const esp_afe_sr_iface_t *esp_afe_handle_from_config(afe_config_t *);
