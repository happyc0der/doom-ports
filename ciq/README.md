# DOOMCE for Garmin Connect IQ

The same raycaster as [`../src/main.c`](../src/main.c), rewritten in Monkey C
for the **Garmin Venu X1** (448×486 AMOLED, Connect IQ API 6). Textured walls,
variable floor and ceiling heights, sliding doors, enemies with the same
state-machine AI, dodgeable projectiles, pickups, status bar. All art is still
generated procedurally at start-up — there are no image resources.

Verified in the Connect IQ simulator: 20 fps (timer-bound), ~400–1000
`fillRectangle` calls per frame, no watchdog trips.

## How the port works, and what the watch taught us

The CE engine never writes pixels; it writes vertical runs of one colour, 8 px
wide (`fill_col8`). That is `dc.fillRectangle`, so the algorithm carried over
unchanged. Then the watch's own numbers (micro-benchmarks written to the device
log — `BENCH` in `Engine.mc`) rewrote the cost model:

| on the Venu X1 | cost |
|---|---|
| one interpreted loop iteration | ~24 µs |
| `fillRectangle`, any size | ~120 µs |
| a method call | ~60 µs |
| `drawScaledBitmap` / `drawBitmap`, any size, to the screen | ~200 µs — **hardware, pixel count is free** |
| drawing into an offscreen `BufferedBitmap` | ~3× slower per call (software) |
| blitting a 448×400 offscreen bitmap to the screen | ~15 ms |
| the watchdog | kills any callback that runs long (~10k iterations in the simulator) |

So the renderer is built around *number of calls*, not pixels:

- **Every textured wall face is one `drawScaledBitmap`** of a prebuilt 1×32
  texture column (4 textures × 8 shade levels × 16 columns = 512 tiny
  bitmaps), drawn at exactly the ray's width. Step and beam faces of 4, 8 or
  12 texels use exact-height columns (768 more) so they need no clip; faces
  under 6 px tall are one flat fill in the texture's average colour. No texel loop, no clip for a
  full-height face, and the GPU scales 32 pixels rather than a whole texture
  that is then clipped away. (The first version drew the full 64-column
  texture clipped to the ray; the GPU work for that showed up as a mystery
  10–30 ms charged to whatever call came next.)
- **Every sprite is one prebuilt palette bitmap per frame**, cropped to its
  opaque box, shaded with `setPalette`, drawn once per run of rays not hidden
  by a wall.
- **The DDA compares a precomputed per-cell key**: the common "nothing changed"
  step is one byte read and one compare. The solid map border replaces the
  bounds checks.
- **Rays are 16 px wide when standing still, 20 px while moving or turning.**
- **The wall pass is cached**: after you stop, it is rendered into an
  offscreen bitmap over four frames (a quarter each, no hitch) and then blitted
  with one call per frame until the view changes. Standing still costs ~12
  calls a frame.
- **Floor and ceiling fills are merged across adjacent rays** when the colour
  matches and the edge is within a pixel: a corridor's ~46 flat fills become
  ~10.
- **The logic tick runs between the wall draws and the sprite draws.** Draw
  calls are asynchronous; the GPU finishes the walls while the CPU does the
  AI, and both walls and sprites use the view state captured at frame start.
- **A frame that overruns the 50 ms timer requests the next frame at once**
  instead of waiting for the next tick, which would round 60 ms up to 100.
- **Things are culled by a 90° cone** before any angle or frame maths; each
  soldier re-checks line of sight every fourth tick, within 12 cells.
- **The HUD is composed into a bitmap when a value changes** and blitted every
  frame — the display is double-buffered, so skipping the HUD on some frames
  makes it blink.
- **Pain is a translucent red border.** Alpha fills are software-blended on
  this watch (~20 ms full-screen), so the border is a ninth of the pixels;
  the muzzle flash is carried by the weapon's own flash frame.
- **Start-up is ~140 small tasks**, a few per timer tick, so nothing trips the
  watchdog; the loading bar covers it.
- **Game logic runs in the timer callback, rendering in `onUpdate`** — two
  callbacks, two watchdog budgets.

Measured on the watch over the last profiling session: **20 fps standing
still** (timer-capped) and **15.6 fps average while moving**, from 1–2 fps for
the straight port. Two kinds of moving frame still dip to 10–12 fps and are
left as they are: views with 60+ wall boundaries (stairs seen edge-on with
beams behind them — ~200 draw calls at ~0.2 ms each), and frames where the
GPU stalls the CPU on the next call after a very tall near-wall column. The
next levers would be adaptive ray width in busy views (a fidelity trade) or
sub-range column bitmaps for near walls (more bitmap objects; the app heap is
at 358 KB of 768).

## Battery

This is a plain **watch-app**: it executes only while it is open on screen. No
background service, no glance, no sensors, no permissions requested. It costs
nothing when you are not playing.

While playing, the cost is the screen at full brightness plus a busy CPU —
expect a few percent per ten minutes. The frame timer (`TICK_MS`, 50 ms) is the
knob: 100 ms halves the CPU cost and is still playable. The CPU idles between
frames; there is no busy loop.

## Build

```sh
brew install --cask connectiq connectiq-sdk-manager
```

Open **SdkManager**, sign in with your Garmin account, and download the
**Venu X1** device under Devices. Then:

```sh
./build.sh          # → bin/DoomCE-venux1.prg   (generates developer_key on first run)
./run.sh            # build, restart the simulator, push, show console for 25 s
```

`run.sh` restarts the simulator every time: `monkeydo` hangs silently if an app
is already running in it. It launches the SDK Manager's copy of the simulator,
not the one the Homebrew cask put in `/Applications` — that one can't find the
SDK's `version.txt` and shows an error dialog on every launch.

## Install on the watch

The Venu X1 is MTP-only over USB, so it does not mount as a drive on macOS.
`tools/mtp_push.c` pushes a file straight into `GARMIN/Apps` with libmtp
(the stock `mtp-sendfile` fails on this watch because it never sets a
storage id):

```sh
brew install libmtp
cd tools && cc -O2 $(pkg-config --cflags --libs libmtp) -o mtp_push mtp_push.c
./mtp_push ../bin/DoomCE-venux1.prg DoomCE.prg
```

Plug the watch in, unlocked, before running it. It replaces any previous copy.
Then unplug, and DOOMCE appears in the app list (developer mode must be on:
Settings → System → About, tap the serial number seven times). No store
submission needed. A GUI alternative is OpenMTP (`brew install --cask openmtp`):
drag the `.prg` into `GARMIN/Apps`.

## Art

`tools/make_art.py` draws the launcher icon and the title logo from
primitives — beveled fire-gradient block letters on a vignette. Run it after
changing either; the PNGs are checked in.

## Controls (Venu X1: two buttons + touch)

| Input | Action |
|---|---|
| **drag left / right** | aim — turns in proportion to the finger's travel |
| tap left / right edge | turn a fixed 13° step |
| tap upper middle / swipe up | forward |
| swipe down | back |
| tap **the gun** (lower middle) / top button | fire |
| tap the status bar | open the door you are facing |
| long-press a move zone | keep moving |
| bottom button | quit |
| any tap after death | restart |

Forward taps accumulate, so mashing walks further. Turning does not
accumulate: earlier builds let turn bursts pile up and the aim overshot.

## Profiling on the watch

Create an empty `GARMIN/Apps/LOGS/DoomCE.TXT` on the device (`tools/mtp_push
/dev/null DoomCE.TXT GARMIN/Apps/LOGS` after `: > /tmp/DoomCE.TXT`) and
`System.println` output is appended to it. With `PROFILE = true` a line every
32 frames gives fps, draw calls, cached frames and per-phase milliseconds
(walls / things / weapon / HUD / tick); `BENCH = true` runs the
micro-benchmarks at start-up. Pull it back with `mtp-getfile <id>` (id from
`mtp-files`).

## Layout

```
manifest.xml            watch-app, venux1 only, no permissions
monkey.jungle
source/DoomCEApp.mc     entry point
source/DoomView.mc      timer, onUpdate, touch/button delegate
source/Engine.mc        the game (tables, art, world, renderer, AI)
resources/              app name + launcher icon
build.sh  run.sh
```
