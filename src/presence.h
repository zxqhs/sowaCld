#ifndef SC_PRESENCE_H
#define SC_PRESENCE_H

#include "config.h"
#include "discord_ipc.h"
#include "track.h"

int presence_send(DiscordIpc *ipc, const Config *cfg, const TrackInfo *t);
int presence_clear(DiscordIpc *ipc);

#endif /* SC_PRESENCE_H */
