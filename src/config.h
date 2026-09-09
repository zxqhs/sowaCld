#ifndef SC_CONFIG_H
#define SC_CONFIG_H

#include <stddef.h>

typedef struct {
    char client_id[32];
    int  poll_ms;
    int  idle_scans_to_clear;
    int  activity_type;
    char activity_name[64];
    char large_image_key[64];
    char small_image_play[64];
    char small_image_pause[64];
    char button_label[33];
    char button_url[256];
    int  log_level;
} Config;

void config_defaults(Config *c);
int  config_load(Config *c, const char *path);
int  config_find_default(char *out, size_t cap);
int  config_client_id_ok(const char *id);

#endif /* SC_CONFIG_H */
