#ifndef SC_DISCORD_IPC_H
#define SC_DISCORD_IPC_H

#include "platform.h"

#include <stddef.h>
#include <stdint.h>

#if defined(SC_WINDOWS)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

enum {
    IPC_OP_HANDSHAKE = 0,
    IPC_OP_FRAME     = 1,
    IPC_OP_CLOSE     = 2,
    IPC_OP_PING      = 3,
    IPC_OP_PONG      = 4
};

#define IPC_RECV_CAP 65536

typedef struct {
#if defined(SC_WINDOWS)
    HANDLE pipe;
#else
    int    fd;
#endif
    int    connected;
    int    ready;
    unsigned char recvbuf[IPC_RECV_CAP];
    size_t recvn;
    uint64_t nonce;
    char   last_evt[64];
} DiscordIpc;

void     discord_ipc_init(DiscordIpc *ipc);
void     discord_ipc_close(DiscordIpc *ipc);
int      discord_ipc_connected(const DiscordIpc *ipc);
int      discord_ipc_connect(DiscordIpc *ipc, const char *client_id);
int      discord_ipc_pump(DiscordIpc *ipc, int timeout_ms);
int      discord_ipc_send_json(DiscordIpc *ipc, const char *json);
uint64_t discord_ipc_next_nonce(DiscordIpc *ipc);

#endif /* SC_DISCORD_IPC_H */
