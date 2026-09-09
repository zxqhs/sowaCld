#!/bin/sh
# Fallback if make complains about separators (tabs vs spaces).
set -e
cd "$(dirname "$0")"
CC="${CC:-cc}"
$CC -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE -DSC_LINUX \
    -o soundcloud-rpc \
    src/main.c src/log.c src/config.c src/json.c src/track.c \
    src/util.c src/discord_ipc.c src/presence.c src/scapi.c src/browser_linux.c \
    -lX11
chmod +x soundcloud-rpc
echo "built ./soundcloud-rpc"
./soundcloud-rpc --self-test
