# План: SoundCloud RPC для Discord (C)

Локальный демон на чистом C. Если в браузере открыт `soundcloud.com` — в Discord показывается Rich Presence. Трека нет — всё равно идёт статус «на SoundCloud». Вкладка закрыта — статус снимается.

Официального API «сейчас играет» у SoundCloud нет. Расширение браузера не требуется: источник правды — заголовок окна браузера (и на Linux дополнительно MPRIS, если браузер его отдаёт).

---

## 1. Цель продукта

В профиле Discord:

```
Listening to SoundCloud
Never Gonna Give You Up
by Rick Astley
──────────────●────────
SoundCloud
[ Open Track ]
```

Минимальный режим (сайт открыт, трек из заголовка не разобрать):

```
Listening to SoundCloud
Browsing
soundcloud.com
```

Правило запуска отслеживания (жёсткое требование):

```
есть окно браузера, в заголовке которого видно SoundCloud
        → RPC активен
иначе
        → RPC очищен
```

Никакого чтения памяти браузера, никакого MITM, никаких расширений. Только публичные заголовки окон + Discord IPC на этой же машине.

---

## 2. Ограничения и решения

| Ограничение | Решение |
|---|---|
| Язык — C, не C++ | C11, без STL, без Game SDK |
| `discord-rpc` deprecated | Свой клиент Discord IPC (pipe / unix socket) |
| Нет официального SoundCloud ↔ Discord | Парсинг заголовка вкладки |
| Artwork трека без URL недоступен | Статичный логотип SoundCloud в Discord Application assets |
| `Listening to` раньше игнорировался | `activity.type = 2`; если клиент отвергнет — fallback type 0 (`Playing`) |
| Имя активности берётся из Discord App | Пользователь создаёт приложение с именем **SoundCloud** |
| Wayland не отдаёт заголовки чужих окон | Linux: X11 (`_NET_WM_NAME`) + MPRIS; если оба пусты — честный лог «нет доступа к окнам» |

Целевые ОС первой поставки: **Linux (X11)** и **Windows**. macOS — не в v1.

Браузеры: Chrome / Chromium / Edge / Brave / Firefox / LibreWolf. Заголовок окна = заголовок активной вкладки.

---

## 3. Архитектура

```
                    ┌──────────────────────┐
                    │  Браузер             │
                    │  soundcloud.com      │
                    │  title = "Track by   │
                    │  Artist \| SoundCloud"│
                    └──────────┬───────────┘
                               │ EnumWindows / X11 / MPRIS
                               ▼
┌─────────────────────────────────────────────────────────┐
│  soundcloud-rpc  (один процесс, один поток + poll)      │
│                                                         │
│  browser_scan  →  title_parse  →  presence_diff         │
│       │                                 │               │
│       │ нет SoundCloud                  │ изменилось    │
│       ▼                                 ▼               │
│  discord_ipc_clear              discord_ipc_set         │
│                                                         │
│  config.ini   discord-ipc-0..9   stdout лог             │
└─────────────────────────────────────────────────────────┘
                               │
                               ▼
                    ┌──────────────────────┐
                    │  Discord Desktop     │
                    │  Rich Presence       │
                    └──────────────────────┘
```

Один процесс, синхронный цикл `sleep(poll_ms)`. Потоки не нужны: IPC и сканирование окон быстрые. Переподключение к Discord — в том же цикле.

---

## 4. Детект SoundCloud

Окно считается вкладкой SoundCloud, если заголовок (без учёта регистра) содержит хотя бы один маркер:

```
soundcloud
```

Ложные срабатывания редки (`SoundCloud` в заголовке почти всегда сам сайт). Дополнительный стоп-лист не нужен в v1.

Приоритет окон, если их несколько:

1. Заголовок парсится как трек (`… by … | SoundCloud` / `… by … | Free Listening on SoundCloud`).
2. Иначе любое окно с маркером — режим Browsing.
3. Linux: если MPRIS браузера отдаёт `xesam:url` с `soundcloud.com` или artist/title, и при этом есть SoundCloud-окно — метаданные MPRIS имеют приоритет (play/pause, чище title/artist).

Состояния трекера:

```
IDLE          нет SoundCloud-окна          → clear presence
BROWSING      сайт открыт, трек не ясен    → details=Browsing, state=soundcloud.com
PLAYING       трек разобран                → details=title, state=by artist
PAUSED        Linux MPRIS PlaybackStatus
              = Paused (и сайт открыт)     → тот же трек + small_text Paused
```

Снятие статуса: 2 подряд идущих скана без SoundCloud-окна (антидребезг при смене вкладки). Discord закрыт — IPC reconnect каждые 5 с, локальный скан не останавливается.

---

## 5. Парсер заголовка

Реальные форматы вкладок SoundCloud:

```
{Title} by {Artist} | SoundCloud
{Title} by {Artist} | Free Listening on SoundCloud
{Title} by {Artist} | Stream {Title} playlist on SoundCloud
Stream and listen to music online for free with SoundCloud
SoundCloud - Hear the world’s sounds
{Artist} | SoundCloud          (профиль)
```

Алгоритм `parse_title(raw) → TrackInfo`:

1. Обрезать суффиксы `| …SoundCloud…`, `- Hear the world’s sounds`.
2. Если остаток пустой / равен `SoundCloud` / начинается с `Stream and listen` → `kind = BROWSING`.
3. Найти последнее ` by ` (регистр-независимо). Слева — title, справа — artist. Оба непустые → `kind = PLAYING`.
4. Иначе `kind = BROWSING`, в `details` положить обрезанный заголовок (профиль, плейлист), обрезав до 128 символов.

Экранирование JSON: `"`, `\`, управляющие → `\uXXXX`. Discord лимиты: `details`/`state` ≤ 128, `large_text` ≤ 128, кнопка label ≤ 32.

Смена трека: strcmp title+artist. Timestamp `start` ставится только при смене трека (полоса прогресса без длительности — «слушает с момента X», без конца). Длительность из заголовка не достать — `timestamps.end` в v1 нет.

---

## 6. Discord IPC (свой, на C)

Транспорт:

| ОС | Путь |
|---|---|
| Linux | `$XDG_RUNTIME_DIR/discord-ipc-N`, иначе `$TMPDIR`, `/run/user/$UID`, `/tmp`. N = 0..9 |
| Windows | `\\.\pipe\discord-ipc-N` |

Фрейм (little-endian):

```
uint32 opcode
uint32 length
uint8  payload[length]     // UTF-8 JSON, без NUL в длине
```

Opcodes: `0 HANDSHAKE`, `1 FRAME`, `2 CLOSE`, `3 PING`, `4 PONG`.

Handshake:

```json
{"v":1,"client_id":"<APPLICATION_ID>"}
```

Ждём `DISPATCH READY`. Дальше только `SET_ACTIVITY`.

SET_ACTIVITY (PLAYING):

```json
{
  "nonce": "<монотонный счётчик>",
  "cmd": "SET_ACTIVITY",
  "args": {
    "pid": <pid демона>,
    "activity": {
      "type": 2,
      "details": "Never Gonna Give You Up",
      "state": "by Rick Astley",
      "timestamps": { "start": 1710000000 },
      "assets": {
        "large_image": "soundcloud",
        "large_text": "SoundCloud",
        "small_image": "play",
        "small_text": "Playing"
      },
      "buttons": [
        { "label": "Open SoundCloud", "url": "https://soundcloud.com" }
      ]
    }
  }
}
```

Clear: `"activity": null`.

JSON пишется вручную (`json_escape` + `snprintf` в буфер 8–16 КБ). Сторонних JSON-библиотек нет — payload простой и фиксированный.

Рейт-лимит Discord: не чаще 1 SET_ACTIVITY / 1 с. Локальный poll 2000 мс, отправка только при diff (kind/title/artist/paused) или heartbeat каждые 30 с (чтобы Discord не забыл activity).

Пинг: если сокет читается и пришёл PING — отвечаем PONG. Если `recv`/`ReadFile` вернул 0 — reconnect.

---

## 7. Модули и файлы

```
soundcloud-rpc/
├── PLAN.md                 ← этот документ
├── README.md               ← сборка, Discord Application, config
├── Makefile
├── config.example.ini
├── src/
│   ├── main.c              цикл, сигналы, аргументы
│   ├── config.c / config.h CLIENT_ID, poll_ms, лог-уровень
│   ├── discord_ipc.c / .h  connect, handshake, send, recv, set, clear
│   ├── json.c / json.h     экранирование и сборка маленьких объектов
│   ├── track.c / track.h   parse_title, TrackInfo, equal
│   ├── presence.c / .h     TrackInfo → SET_ACTIVITY / clear
│   ├── log.c / log.h       timestamp + уровень
│   ├── platform.h          SC_WINDOWS / SC_LINUX
│   ├── browser_win.c       EnumWindows + GetWindowTextW (UTF-16→UTF-8)
│   ├── browser_linux.c     X11 _NET_CLIENT_LIST + _NET_WM_NAME
│   └── mpris_linux.c       опционально, libdbus-1 (можно выключить)
├── include/                публичных заголовков нет, всё в src/
└── assets/
    └── soundcloud.png      логотип 512×512 — загрузить в Dev Portal как key `soundcloud`
                            play.png / pause.png — small assets
```

Граница платформ: `browser_scan(TrackInfo *out)` объявлен в `browser.h`, реализация одна на ОС. `main.c` платформонезависим.

Зависимости:

- Linux: `libx11`, опционально `libdbus-1`
- Windows: только `kernel32`, `user32`, `ws2_32` не нужен (named pipe)
- Общее: libc. Без curl, без openssl.

---

## 8. Конфиг и CLI

`config.ini` рядом с бинарём, либо `~/.config/soundcloud-rpc/config.ini`:

```ini
client_id = 000000000000000000
poll_ms = 2000
idle_scans_to_clear = 2
activity_type = 2
large_image_key = soundcloud
button_label = Open SoundCloud
button_url = https://soundcloud.com
log_level = info
```

CLI:

```
soundcloud-rpc                 # демон на переднем плане
soundcloud-rpc -c path.ini
soundcloud-rpc --dry-run       # только печать найденного трека, без Discord
soundcloud-rpc -v
```

`client_id` обязателен. Без него — ошибка с инструкцией создать приложение на https://discord.com/developers/applications (имя: `SoundCloud`, Asset: `soundcloud`).

Сигналы Linux: SIGINT/SIGTERM → clear activity → close socket → exit 0. Windows: `SetConsoleCtrlHandler` аналогично.

---

## 9. Цикл main (псевдокод)

```
load_config()
install_signal_handlers()
last = EMPTY
misses = 0
connected = false

while running:
    if not connected:
        connected = discord_connect_and_handshake(client_id)
        if not connected:
            sleep(5000); continue

    discord_pump_ping()          # non-blocking read

    info = browser_scan()        # + mpris_linux если есть

    if info.kind == NONE:
        misses++
        if misses >= idle_scans_to_clear and last.kind != NONE:
            discord_clear()
            last = EMPTY
    else:
        misses = 0
        if !track_equal(info, last) or heartbeat_due:
            if info.kind == PLAYING and title changed:
                info.start_unix = now()
            else:
                info.start_unix = last.start_unix
            discord_set(info)
            last = info

    sleep(poll_ms)

discord_clear()
discord_close()
```

`browser_scan` на Linux:

1. `XOpenDisplay` один раз при старте.
2. `_NET_CLIENT_LIST` → для каждого окна `_NET_WM_NAME` (UTF8) fallback `WM_NAME`.
3. Фильтр по маркеру `soundcloud`.
4. Первый PLAYING, иначе первый BROWSING.
5. Дополнительно MPRIS: список имён на `org.freedesktop.DBus`, префиксы `org.mpris.MediaPlayer2.chrome`, `.firefox`, `.chromium`, `.brave`, `.edge`. Читать `Metadata` + `PlaybackStatus`. Использовать только если Identity/url/title намекают на SoundCloud **и** шаг 3 уже нашёл окно.

Windows `browser_scan`:

1. `EnumWindows`.
2. Видимые окна, `GetClassName` ∈ `{Chrome_WidgetWin_1, MozillaWindowClass, Chrome_WidgetWin_1 (Edge)}` — не жёсткий фильтр, достаточно заголовка.
3. `GetWindowTextW` → UTF-8.

---

## 10. Сборка

Linux:

```makefile
CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -D_POSIX_C_SOURCE=200809L
LIBS = -lX11
# опционально: CFLAGS += -DHAVE_MPRIS  LIBS += $(shell pkg-config --libs dbus-1)

soundcloud-rpc: $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LIBS)
```

Windows (MinGW):

```makefile
CC = x86_64-w64-mingw32-gcc
CFLAGS = -std=c11 -O2 -Wall -DUNICODE -D_UNICODE
LIBS = -luser32 -lkernel32
```

Без cmake в v1. Один Makefile с `ifeq ($(OS),Windows_NT)`.

---

## 11. Этапы реализации (порядок работы)

Делать строго по шагам. Каждый шаг должен компилироваться.

### Этап 0 — каркас
- Репозиторий, Makefile, `log.c`, `config.c`, `main.c` с `--dry-run` заглушкой.
- `config.example.ini`, README: как создать Discord Application, загрузить asset `soundcloud`.

### Этап 1 — Discord IPC
- `discord_ipc.c`: connect 0..9, handshake, FRAME send/recv, PING/PONG.
- Тестовый режим `--test-rpc`: выставить фиктивный `details=Test`, подождать Enter, clear, выход.
- Проверка руками в Discord.

### Этап 2 — JSON и presence
- `json_escape`, сборка SET_ACTIVITY / clear.
- `activity.type`, assets, buttons, timestamps.
- Heartbeat 30 с.

### Этап 3 — парсер заголовков
- Таблица unit-проверок в `track.c` (`--self-test`): 8–10 фиксированных строк → ожидаемый kind/title/artist.
- Граничные случаи: нет `by`, несколько `by`, длинные строки, кавычки.

### Этап 4 — сканер браузера
- Linux X11.
- Windows EnumWindows.
- `--dry-run` печатает каждое SoundCloud-окно и выбранный TrackInfo раз в poll.

### Этап 5 — склейка
- Цикл из §9.
- Антидребезг, смена трека → новый timestamp, clear при закрытии.
- SIGINT чистит presence.

### Этап 6 — Linux MPRIS (опционально, `#ifdef HAVE_MPRIS`)
- Pause/play small asset.
- Если MPRIS недоступен — поведение как на Windows.

### Этап 7 — полировка
- README, пример конфига, иконки в `assets/` (сгенерировать).
- Логотип не копировать с сайта SoundCloud 1-в-1 (торговая марка): нарисовать нейтральный оранжевый облачный знак в стиле, ключ `soundcloud`.
- Обработка: Discord не запущен, нет X11, пустой client_id, обрыв pipe.

Критерий готовности v1:

1. Открыл soundcloud.com — в Discord появился статус.
2. Включил трек — title/artist обновились.
3. Закрыл вкладку — статус пропал за ≤ 2×poll.
4. Discord перезапустили — демон сам переподключился.
5. `--dry-run` работает без Discord.

---

## 12. Что сознательно не делаем в v1

- Браузерное расширение / localhost HTTP от content script.
- Скрейп HTML / DevTools Protocol / remote debugging.
- Обложки треков через SoundCloud oEmbed (нужен URL, из title его нет).
- Кнопка «открыть именно этот трек» (нет permalink).
- `timestamps.end` и точный прогресс.
- Трей в системном баре, автостарт systemd/Task Scheduler — можно дописать после v1.
- macOS.
- Wayland без XWayland: если `_NET_WM_NAME` пуст, остаётся только MPRIS.

Эти пункты — кандидаты в v2, не блокируют поставку.

---

## 13. Риски

| Риск | Митигация |
|---|---|
| SoundCloud сменит формат `<title>` | Парсер эвристический + `--dry-run` для отладки; поправить `track.c` |
| Discord отвергнет `type: 2` | `activity_type` в конфиге, дефолт 2, fallback 0 |
| Asset `soundcloud` не загружен | Presence без картинки всё равно работает; лог-предупреждение не требуется (Discord просто не рисует) |
| Несколько окон SoundCloud | Берём первое PLAYING |
| Браузер в sandbox / другой user | Не увидим окна — `--dry-run` это покажет |
| IPC занят другим RPC с тем же client_id | Один client_id = один presence; в README: не запускать PreMiD параллельно на том же App ID |

---

## 14. Discord Application (ручной шаг пользователя)

1. https://discord.com/developers/applications → New Application → имя **SoundCloud**.
2. Settings → Rich Presence → Art Assets: загрузить `soundcloud` (512×512), опционально `play` / `pause`.
3. Скопировать Application ID в `config.ini` как `client_id`.
4. Discord Desktop должен быть запущен под тем же пользователем ОС.

Без этого шага IPC handshake проходит только если ID валидный. В репозиторий никакой чужой client_id не кладём.

---

## 15. Порядок после утверждения плана

Сразу после этого документа:

1. Каркас + Makefile + README.
2. IPC + `--test-rpc`.
3. Парсер + сканер + основной цикл.
4. Прогон `--dry-run`, затем живой Discord.

Стек фиксирован: C11, свой IPC, X11/Win32, ini-конфиг, без расширения браузера.
