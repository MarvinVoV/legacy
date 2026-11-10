#pragma once
typedef struct { int num; } srmodel_list_t;
srmodel_list_t *esp_srmodel_init(const char*);
char *esp_srmodel_filter(srmodel_list_t*,const char*,const char*);
void esp_srmodel_deinit(srmodel_list_t*);
