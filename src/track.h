#ifndef SC_TRACK_H
#define SC_TRACK_H

#include <stdint.h>

typedef enum {
    TRACK_NONE = 0,
    TRACK_BROWSING,
    TRACK_PLAYING
} TrackKind;

typedef struct {
    TrackKind kind;
    char      title[129];
    char      artist[129];
    char      details[129]; /* BROWSING leftover, or copy of title */
    char      raw[512];
    char      url[512];     /* permalink https://soundcloud.com/... */
    char      art_url[512]; /* https artwork for Discord large_image */
    int       paused;
    int       searching;
    int64_t   start_ms;
} TrackInfo;

void track_clear(TrackInfo *t);
void track_parse(const char *raw_title, TrackInfo *out);
int  track_equal(const TrackInfo *a, const TrackInfo *b);
int  track_same_song(const TrackInfo *a, const TrackInfo *b);
int  track_is_soundcloud_title(const char *title);
void track_log(const TrackInfo *t, const char *prefix);
int  track_self_test(void);

#endif /* SC_TRACK_H */
