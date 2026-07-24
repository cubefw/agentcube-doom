# agentcube-doom

**Doomgeneric** port with a platform hook that streams every frame to an [AgentCube](https://github.com/a2u/agentcube-sim) display (simulator or real cube).

```
Doom (PC) → scale 240×240 → RGB565 → POST /api/v1/draw/frame → AgentCube
```

Based on [ozkl/doomgeneric](https://github.com/ozkl/doomgeneric). Local platform file: `doomgeneric_agentcube.c`.

## Why doomgeneric

Small platform API (`DG_DrawFrame`, `DG_GetKey`, …) — ideal for “PC plays, cube is the monitor”.

## Requirements (macOS)

```bash
brew install sdl2 curl
# optional sound:
# brew install sdl2_mixer
# make -f Makefile.agentcube WITH_SOUND=1
```

You need a Doom **IWAD** (not included). Shareware:

```bash
# example (check URL still works)
curl -L -o wads/doom1.wad \
  "https://distro.ibiblio.org/pub/linux/distributions/slitaz/sources/packages/d/doom1.wad"
```

Or copy your legal `doom1.wad` / `doom2.wad` into `wads/`.

## Build

```bash
cd doomgeneric-src/doomgeneric
make -f Makefile.agentcube -j$(sysctl -n hw.ncpu)
# binary: ./doomgeneric-agentcube
```

Or from repo root:

```bash
./scripts/build-macos.sh
```

## Run with AgentCube simulator

Terminal 1 — sim:

```bash
cd /path/to/agentcube-sim   # or geekmagic/simulator
python3 agentcube_sim.py
# open http://127.0.0.1:8765/screen
```

Terminal 2 — Doom:

```bash
export AGENTCUBE_HOST=127.0.0.1:8765
./doomgeneric-src/doomgeneric/doomgeneric-agentcube \
  -iwad wads/doom1.wad \
  -agentcube 127.0.0.1:8765
```

## Run on a real cube (ESP8266 / GeekMagic)

### High FPS — ACSP atomic present (default)

Firmware **TCP :81**: each frame is received fully into RAM (**80×80 RGB565**), while the **previous picture stays on the LCD**. Only then a fast SPI **3× upscale** paints 240×240. That is real double-buffer semantics on ESP8266 (full 240×240 backbuffer does not fit in RAM).

Measured ~**9 FPS** solid color / test pattern on LAN.

```bash
cd /Users/dandare/Work/agentcube-doom
(cd doomgeneric-src/doomgeneric && make -f Makefile.agentcube -j$(sysctl -n hw.ncpu))

# Same atomic path as video: 80×80 RGB565 → TCP :81 → cube 3× present
./doomgeneric-src/doomgeneric/doomgeneric-agentcube \
  -iwad wads/DOOM1.WAD \
  -agentcube 192.168.1.97 \
  -frameskip 1 \
  -nowindow
```

In stderr: `ACSP TCP connected …` and `~N FPS stream (80x80 ACSP)`.
Picture stays on the cube until the next full frame is received (no black flash).

### Stream a video file (easiest visual test)

```bash
brew install ffmpeg   # once

# moving test pattern (no file)
python3 scripts/stream_video_to_cube.py -H 192.168.1.97 --test

# any mp4/mkv/webm
python3 scripts/stream_video_to_cube.py -H 192.168.1.97 -i ~/Movies/clip.mp4 --fps 12

# loop
python3 scripts/stream_video_to_cube.py -H 192.168.1.97 -i clip.mp4 --fps 15 --loop
```

Discovery: `curl http://192.168.1.97/api/v1/stream/info`
### Legacy HTTP strips (fallback)

If TCP fails, the client falls back to `POST /api/v1/draw/frame` with **H=8** strips. Do **not** use large `-striph` (OOM reboot).

Quick API smoke test (no Doom):

```bash
# red strip at top
python3 - <<'PY'
import urllib.request, struct
w,h=240,20
body=struct.pack('<H', 0xF800)*(w*h)
req=urllib.request.Request(
  'http://192.168.1.97/api/v1/draw/frame', data=body, method='POST',
  headers={'Content-Type':'application/octet-stream',
           'X-Frame-X':'0','X-Frame-Y':'0','X-Frame-W':str(w),'X-Frame-H':str(h)})
print(urllib.request.urlopen(req, timeout=5).read())
PY
```

### Options

| Flag / env | Meaning |
|------------|---------|
| `-agentcube host:port` | Stream target (default `127.0.0.1:8765` or `AGENTCUBE_HOST`) |
| `-nostream` | Only local SDL window |
| `-nowindow` | Hidden SDL window (still needs video for events) |
| `-frameskip N` | Send every Nth frame (default 1) |
| `-striph N` | Strip height in lines (default 20, max 24) |
| `AGENTCUBE_FRAME_SKIP` | Same as frameskip |
| `AGENTCUBE_STRIP_H` | Same as striph |
| `AGENTCUBE_STREAM=0` | Disable stream |

Controls: arrows, Ctrl fire, Space use, Esc menu (classic Doom).

## Pipeline

1. Engine fills `DG_ScreenBuffer` (RGB888 `uint32`, default **640×400**).
2. `DG_DrawFrame` presents to SDL (optional).
3. Nearest-neighbour scale → **240×240** RGB565 LE (in host RAM).
4. Stream as **horizontal strips** via `POST /api/v1/draw/frame` with headers  
   `X-Frame-X/Y/W/H` (body = `W*H*2` bytes, ≤ ~12 KB on ESP8266).
## Layout

```
agentcube-doom/
  README.md
  scripts/build-macos.sh
  wads/                 # gitignored — put IWAD here
  doomgeneric-src/      # vendored doomgeneric + our platform
    doomgeneric/
      doomgeneric_agentcube.c
      Makefile.agentcube
      doomgeneric-agentcube   # build output
```

## License

- Doomgeneric / Doom code: see `doomgeneric-src/LICENSE` (GPL-compatible Doom heritage).
- AgentCube platform glue in `doomgeneric_agentcube.c`: MIT (this project).
