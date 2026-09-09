CC     ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE
LDFLAGS ?=

SRC_COMMON = src/main.c src/log.c src/config.c src/json.c src/track.c \
             src/util.c src/discord_ipc.c src/presence.c src/scapi.c

ifeq ($(OS),Windows_NT)
  TARGET  = soundcloud-rpc.exe
  CFLAGS += -DSC_WINDOWS -DUNICODE -D_UNICODE
  LIBS    = -luser32 -lkernel32
  SRC     = $(SRC_COMMON) src/browser_win.c
else
  TARGET  = soundcloud-rpc
  CFLAGS += -DSC_LINUX
  LIBS    = -lX11
  SRC     = $(SRC_COMMON) src/browser_linux.c
  HAVE_MPRIS ?= $(shell pkg-config --exists dbus-1 && echo 1 || echo 0)
  ifeq ($(HAVE_MPRIS),1)
    CFLAGS += -DHAVE_MPRIS $(shell pkg-config --cflags dbus-1)
    LIBS   += $(shell pkg-config --libs dbus-1)
    SRC    += src/mpris_linux.c
  endif
endif

# '>' instead of tab — survives copy/paste and editors that convert tabs to spaces
.RECIPEPREFIX := >

.PHONY: all clean test rebuild

all: $(TARGET)
>chmod +x $(TARGET)

$(TARGET): $(SRC)
>$(CC) $(CFLAGS) -o $@ $(SRC) $(LIBS) $(LDFLAGS)
>chmod +x $@

clean:
>rm -f soundcloud-rpc soundcloud-rpc.exe

rebuild: clean all

test: $(TARGET)
>chmod +x $(TARGET)
>./$(TARGET) --self-test
