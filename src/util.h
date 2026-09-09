#ifndef SC_UTIL_H
#define SC_UTIL_H

#include <stddef.h>
#include <stdint.h>

void     sc_strlcpy(char *dst, const char *src, size_t cap);
void     sc_trim(char *s);
int      sc_strcasecmp(const char *a, const char *b);
int      sc_istartswith(const char *s, const char *prefix);
char    *sc_strcasestr(const char *hay, const char *needle);
void     sc_utf8_clamp(char *s, size_t max_bytes);
int64_t  sc_now_ms(void);
void     sc_sleep_ms(int ms);
int      sc_is_http_url(const char *s);
int      sc_is_track_url(const char *s);
void     sc_upgrade_art_url(char *url);
int      sc_http_get(const char *url, char *out, size_t cap);
int      sc_json_string(const char *json, const char *key, char *out, size_t cap);
void     sc_urlenc(char *dst, size_t cap, const char *src);

#endif /* SC_UTIL_H */
