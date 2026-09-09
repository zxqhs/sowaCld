#ifndef SC_BROWSER_H
#define SC_BROWSER_H

#include "track.h"

int  browser_init(void);
void browser_shutdown(void);
int  browser_scan(TrackInfo *out);
void browser_enrich(TrackInfo *t);

#ifdef HAVE_MPRIS
int  mpris_init(void);
void mpris_shutdown(void);
int  mpris_query(TrackInfo *out);
#endif

#endif /* SC_BROWSER_H */
