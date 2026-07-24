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

The cube only accepts **strips** of RGB565 (max ~12 KB/request). This client sends horizontal bands (default **H=16** → 7680 bytes, 15 POSTs per frame).

```bash
cd /Users/dandare/Work/agentcube-doom   # or your clone
# build once
cd doomgeneric-src/doomgeneric && make -f Makefile.agentcube -j$(sysctl -n hw.ncpu) && cd ../..

# stream to cube LAN IP (token not required on open AgentCube builds)
export AGENTCUBE_HOST=192.168.1.97
export AGENTCUBE_FRAME_SKIP=2          # optional: less Wi‑Fi load
# export AGENTCUBE_STRIP_H=12          # optional: thinner strips if Wi‑Fi drops

./doomgeneric-src/doomgeneric/doomgeneric-agentcube \
  -iwad wads/DOOM1.WAD \
  -agentcube 192.168.1.97 \
  -frameskip 2
```

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
