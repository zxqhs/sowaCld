#ifndef SC_SCAPI_H
#define SC_SCAPI_H

#include "track.h"

/* Fill t->url and t->art_url via SoundCloud search when missing. */
void scapi_lookup(TrackInfo *t);
void scapi_warm(void);

#endif
