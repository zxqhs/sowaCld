# soundcloud-rpc

Local **C11** daemon: if SoundCloud is open in a browser, Discord shows Rich Presence — track title, artist, artwork, and a permalink.

No browser extension. Unofficial, not affiliated with SoundCloud or Discord.

```
Playing sowaCld
desire
SNW
[ Open Track ]
```

## Features

- Track title + artist (no `by` prefix)
- Cover art as Discord `large_image` (oEmbed / SoundCloud search)
- One **Open Track** button (permalink) — Discord hides buttons on *your own* profile; others still see them. Title, artist, and cover are clickable for you via `details_url` / `state_url` / `large_url`
- Search without playback → `Searching: 'query'`
- Only reports **Playing** when playerctl/MPRIS says `Playing`/`Paused` (tab title alone is not enough)
- Application name `sowaCld` (set it in the Discord Developer Portal too)
- Linux (X11 + playerctl + optional MPRIS) and Windows

## Build

**Linux**

```bash
# Arch
sudo pacman -S gcc make libx11 curl playerctl

# Debian/Ubuntu
sudo apt install build-essential libx11-dev curl playerctl

make rebuild
./soundcloud-rpc --self-test
```

Optional MPRIS (pause/play + cleaner metadata). Enabled automatically if `libdbus-1` is found:

```bash
# Debian/Ubuntu
sudo apt install libdbus-1-dev
make rebuild
```

If `make` complains about missing separators, recipes use `>` instead of Tab — copy `Makefile` as-is, or:

```bash
sh build.sh
```

**Windows (MinGW)**

```bash
make
```

## Discord Application

1. [Discord Developer Portal](https://discord.com/developers/applications) → **New Application**. Name it **sowaCld** (this is the “Playing …” label).
2. Copy **Application ID**.
3. **Rich Presence → Art Assets** — upload from `assets/`:
   - `soundcloud.png` → key `soundcloud` (fallback cover)
   - `play.png` → key `play`
   - `pause.png` → key `pause`
4. Run **Discord desktop** as the same OS user. Do not run another RPC (PreMiD, CustomRP, …) with the same Application ID.

## Config

```bash
cp config.example.ini config.ini
# paste client_id
```

Lookup order: `./config.ini`, then `~/.config/soundcloud-rpc/config.ini`.

```ini
client_id = 123456789012345678
poll_ms = 2000
activity_type = 0          # 0 = Playing, 2 = Listening to
activity_name = sowaCld
button_label = Open Track
log_level = info
```

While a track is playing or you are searching, the daemon polls every ~300–400 ms. `poll_ms` is the idle interval.

## Run

```bash
./soundcloud-rpc                 # foreground daemon
./soundcloud-rpc -c path.ini
./soundcloud-rpc --dry-run -v    # print detected tracks, no Discord
./soundcloud-rpc --test-rpc      # dummy presence, then Enter
./soundcloud-rpc --self-test
./soundcloud-rpc -h
```

Open [soundcloud.com](https://soundcloud.com), play a track. Status should appear in Discord.

Ctrl+C / SIGTERM clears presence and exits.

**Own profile:** Discord often freezes the profile card. Close and reopen it, or check the member-list / user popout. Buttons never show on your own profile (Discord limitation).

## How it works

1. Scan browser window titles (X11 `_NET_WM_NAME` / Win32 `EnumWindows`, plus hyprctl/sway/niri/wmctrl on Linux).
2. Read live playback from **playerctl** / **MPRIS** (`Playing` / `Paused` only).
3. Resolve artwork + permalink via SoundCloud oEmbed / search (`curl`).
4. Talk to Discord over IPC (`$XDG_RUNTIME_DIR/discord-ipc-N` or `\\.\pipe\discord-ipc-N`), `SET_ACTIVITY`.

A tab is treated as SoundCloud only with a real site marker (`| SoundCloud`, `results on SoundCloud`, …). YouTube/Google titles that merely mention the word “soundcloud” are ignored.

## Limits

- No exact progress bar (no duration in the title).
- Pause on Windows is not visible from the title; on Linux it needs MPRIS/playerctl.
- Pure Wayland without XWayland may not expose other windows’ titles — playerctl/MPRIS still work.
- No macOS.
- Discord rate-limits `SET_ACTIVITY`; rapid skips are coalesced to the last settled track.
- Artwork URLs are fetched by Discord’s CDN and can lag on first show.

## License

MIT. Images in `assets/` were drawn from scratch; they are not SoundCloud trademarks. You choose the Discord application name yourself.
