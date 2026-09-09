#include "discord_ipc.h"
#include "log.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(SC_WINDOWS)
#  include <stdio.h>
#else
#  include <dirent.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/un.h>
#  include <unistd.h>
#endif

static uint32_t rd_le32(const unsigned char *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void wr_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

void discord_ipc_init(DiscordIpc *ipc)
{
    memset(ipc, 0, sizeof *ipc);
#if defined(SC_WINDOWS)
    ipc->pipe = INVALID_HANDLE_VALUE;
#else
    ipc->fd = -1;
#endif
}

void discord_ipc_close(DiscordIpc *ipc)
{
#if defined(SC_WINDOWS)
    if (ipc->pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(ipc->pipe);
        ipc->pipe = INVALID_HANDLE_VALUE;
    }
#else
    if (ipc->fd >= 0) {
        close(ipc->fd);
        ipc->fd = -1;
    }
#endif
    ipc->connected = 0;
    ipc->ready = 0;
    ipc->recvn = 0;
}

int discord_ipc_connected(const DiscordIpc *ipc)
{
    return ipc->connected && ipc->ready;
}

uint64_t discord_ipc_next_nonce(DiscordIpc *ipc)
{
    return ++ipc->nonce;
}

#if defined(SC_WINDOWS)

static int ipc_write_all(DiscordIpc *ipc, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    size_t off = 0;

    while (off < n) {
        DWORD got = 0;
        if (!WriteFile(ipc->pipe, p + off, (DWORD)(n - off), &got, NULL))
            return -1;
        if (got == 0)
            return -1;
        off += got;
    }
    return 0;
}

static int ipc_read_some(DiscordIpc *ipc)
{
    DWORD avail = 0, got = 0;
    size_t space;

    if (!PeekNamedPipe(ipc->pipe, NULL, 0, NULL, &avail, NULL))
        return -1;
    if (avail == 0)
        return 0;
    space = sizeof ipc->recvbuf - ipc->recvn;
    if (space == 0)
        return -1;
    if (avail > space)
        avail = (DWORD)space;
    if (!ReadFile(ipc->pipe, ipc->recvbuf + ipc->recvn, avail, &got, NULL))
        return -1;
    if (got == 0)
        return -1;
    ipc->recvn += got;
    return 1;
}

#else /* Linux */

static int ipc_write_all(DiscordIpc *ipc, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    size_t off = 0;

    while (off < n) {
        ssize_t got = send(ipc->fd, p + off, n - off, MSG_NOSIGNAL);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd;
                pfd.fd = ipc->fd;
                pfd.events = POLLOUT;
                if (poll(&pfd, 1, 2000) <= 0)
                    return -1;
                continue;
            }
            return -1;
        }
        if (got == 0)
            return -1;
        off += (size_t)got;
    }
    return 0;
}

static int ipc_read_some(DiscordIpc *ipc)
{
    ssize_t got;
    size_t space = sizeof ipc->recvbuf - ipc->recvn;

    if (space == 0)
        return -1;
    got = recv(ipc->fd, ipc->recvbuf + ipc->recvn, space, 0);
    if (got < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    if (got == 0)
        return -1;
    ipc->recvn += (size_t)got;
    return 1;
}

#endif

static int ipc_send_frame(DiscordIpc *ipc, uint32_t opcode, const char *json)
{
    unsigned char hdr[8];
    uint32_t len;

    if (!json)
        json = "";
    len = (uint32_t)strlen(json);
    wr_le32(hdr, opcode);
    wr_le32(hdr + 4, len);
    if (ipc_write_all(ipc, hdr, 8) != 0)
        return -1;
    if (len && ipc_write_all(ipc, json, len) != 0)
        return -1;
    LOG_D("ipc send op=%u len=%u %s", opcode, len, json);
    return 0;
}

static int ipc_pop_frame(DiscordIpc *ipc, uint32_t *op, char *payload, size_t payload_cap, uint32_t *plen)
{
    uint32_t opcode, length;

    if (ipc->recvn < 8)
        return 0;
    opcode = rd_le32(ipc->recvbuf);
    length = rd_le32(ipc->recvbuf + 4);
    if (length > IPC_RECV_CAP - 8)
        return -1;
    if (ipc->recvn < 8u + length)
        return 0;
    if (payload_cap < (size_t)length + 1)
        return -1;
    memcpy(payload, ipc->recvbuf + 8, length);
    payload[length] = '\0';
    memmove(ipc->recvbuf, ipc->recvbuf + 8 + length, ipc->recvn - 8 - length);
    ipc->recvn -= 8u + length;
    *op = opcode;
    *plen = length;
    return 1;
}

static int handle_frame(DiscordIpc *ipc, uint32_t op, const char *payload)
{
    LOG_D("ipc recv op=%u %s", op, payload);
    if (op == IPC_OP_PING) {
        if (ipc_send_frame(ipc, IPC_OP_PONG, payload) != 0)
            return -1;
        return 0;
    }
    if (op == IPC_OP_CLOSE) {
        LOG_W("Discord closed IPC: %s", payload);
        return -1;
    }
    if (op == IPC_OP_FRAME) {
        if (strstr(payload, "\"evt\":\"READY\"") || strstr(payload, "\"evt\": \"READY\"")) {
            ipc->ready = 1;
            sc_strlcpy(ipc->last_evt, "READY", sizeof ipc->last_evt);
        } else if (strstr(payload, "\"evt\":\"ERROR\"") || strstr(payload, "\"code\"")) {
            if (strstr(payload, "ERROR"))
                LOG_W("Discord IPC error: %s", payload);
        }
        return 0;
    }
    return 0;
}

int discord_ipc_pump(DiscordIpc *ipc, int timeout_ms)
{
    char payload[IPC_RECV_CAP];
    uint32_t op, len;
    int pr;

    if (!ipc->connected)
        return -1;

#if defined(SC_WINDOWS)
    {
        int64_t deadline = sc_now_ms() + (timeout_ms > 0 ? timeout_ms : 0);
        for (;;) {
            int r = ipc_read_some(ipc);
            if (r < 0)
                goto fail;
            if (r == 0) {
                if (timeout_ms <= 0)
                    break;
                if (sc_now_ms() >= deadline)
                    break;
                Sleep(10);
                continue;
            }
            timeout_ms = 0; /* drain after first data */
        }
    }
#else
    {
        struct pollfd pfd;
        pfd.fd = ipc->fd;
        pfd.events = POLLIN;
        pr = poll(&pfd, 1, timeout_ms < 0 ? 0 : timeout_ms);
        if (pr < 0) {
            if (errno == EINTR)
                return 0;
            goto fail;
        }
        if (pr > 0) {
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
                goto fail;
            if (ipc_read_some(ipc) < 0)
                goto fail;
            /* drain */
            for (;;) {
                int r;
                pfd.events = POLLIN;
                if (poll(&pfd, 1, 0) <= 0)
                    break;
                r = ipc_read_some(ipc);
                if (r < 0)
                    goto fail;
                if (r == 0)
                    break;
            }
        }
    }
#endif

    for (;;) {
        int got = ipc_pop_frame(ipc, &op, payload, sizeof payload, &len);
        if (got == 0)
            break;
        if (got < 0)
            goto fail;
        if (handle_frame(ipc, op, payload) != 0)
            goto fail;
    }
    return 0;

fail:
    discord_ipc_close(ipc);
    return -1;
}

int discord_ipc_send_json(DiscordIpc *ipc, const char *json)
{
    if (!ipc->connected)
        return -1;
    if (ipc_send_frame(ipc, IPC_OP_FRAME, json) != 0) {
        discord_ipc_close(ipc);
        return -1;
    }
    return 0;
}

#if defined(SC_WINDOWS)

static int try_pipe(DiscordIpc *ipc, int n)
{
    char name[64];
    HANDLE h;
    DWORD mode;

    snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", n);
    h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(h, &mode, NULL, NULL);
    ipc->pipe = h;
    ipc->connected = 1;
    ipc->recvn = 0;
    LOG_D("opened %s", name);
    return 0;
}

static int try_all_sockets(DiscordIpc *ipc)
{
    int n;
    for (n = 0; n < 10; n++) {
        if (try_pipe(ipc, n) == 0)
            return 0;
    }
    return -1;
}

#else

static int try_path(DiscordIpc *ipc, const char *path)
{
    struct sockaddr_un addr;
    int fd, flags;

    if (!path || !path[0])
        return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr.sun_path) {
        close(fd);
        return -1;
    }
    strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    ipc->fd = fd;
    ipc->connected = 1;
    ipc->recvn = 0;
    LOG_I("opened Discord IPC: %s", path);
    return 0;
}

static int try_dir(DiscordIpc *ipc, const char *dir)
{
    int n;
    char path[512];

    if (!dir || !dir[0])
        return -1;
    for (n = 0; n < 10; n++) {
        snprintf(path, sizeof path, "%s/discord-ipc-%d", dir, n);
        if (try_path(ipc, path) == 0)
            return 0;
    }
    return -1;
}

static int is_dir(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int try_dir_walk(DiscordIpc *ipc, const char *dir, int depth)
{
    DIR *d;
    struct dirent *ent;

    if (try_dir(ipc, dir) == 0)
        return 0;
    if (depth <= 0 || !is_dir(dir))
        return -1;
    d = opendir(dir);
    if (!d)
        return -1;
    while ((ent = readdir(d)) != NULL) {
        char sub[512];
        if (ent->d_name[0] == '.')
            continue;
        snprintf(sub, sizeof sub, "%s/%s", dir, ent->d_name);
        if (is_dir(sub) && try_dir_walk(ipc, sub, depth - 1) == 0) {
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

static int try_all_sockets(DiscordIpc *ipc)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *tmp = getenv("TMPDIR");
    char extra[512];
    uid_t uid = getuid();
    static const char *const extra_rel[] = {
        "app/com.discordapp.Discord",
        "app/com.discordapp.DiscordCanary",
        "app/com.discordapp.DiscordPTB",
        "app/dev.vencord.Vesktop",
        "app/dev.vencord.vesktop",
        "app/com.vencord.Vesktop",
        "snap.discord",
        ".flatpak/com.discordapp.Discord/xdg-run",
        ".flatpak/dev.vencord.Vesktop/xdg-run",
        NULL
    };
    int i;

    if (runtime && try_dir_walk(ipc, runtime, 2) == 0)
        return 0;
    if (runtime) {
        for (i = 0; extra_rel[i]; i++) {
            snprintf(extra, sizeof extra, "%s/%s", runtime, extra_rel[i]);
            if (try_dir(ipc, extra) == 0)
                return 0;
        }
    }
    snprintf(extra, sizeof extra, "/run/user/%u", (unsigned)uid);
    if (try_dir_walk(ipc, extra, 2) == 0)
        return 0;
    if (try_dir(ipc, tmp) == 0)
        return 0;
    if (try_dir(ipc, "/tmp") == 0)
        return 0;
    return -1;
}

#endif

int discord_ipc_connect(DiscordIpc *ipc, const char *client_id)
{
    char hs[256];
    int64_t deadline;

    discord_ipc_close(ipc);
    discord_ipc_init(ipc);

    if (try_all_sockets(ipc) != 0) {
        LOG_W("no discord-ipc-* socket found");
        LOG_W("start Discord desktop (not the browser). Vesktop: enable Rich Presence / arRPC");
        return -1;
    }

    snprintf(hs, sizeof hs, "{\"v\":1,\"client_id\":\"%s\"}", client_id);
    if (ipc_send_frame(ipc, IPC_OP_HANDSHAKE, hs) != 0) {
        discord_ipc_close(ipc);
        return -1;
    }

    deadline = sc_now_ms() + 5000;
    while (sc_now_ms() < deadline && !ipc->ready) {
        int left = (int)(deadline - sc_now_ms());
        if (left < 1)
            left = 1;
        if (discord_ipc_pump(ipc, left > 200 ? 200 : left) != 0)
            return -1;
    }
    if (!ipc->ready) {
        LOG_W("handshake timed out");
        discord_ipc_close(ipc);
        return -1;
    }
    return 0;
}
