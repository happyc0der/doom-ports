# DOOMCE for Garmin Connect IQ

The same raycaster as [`../src/main.c`](../src/main.c), rewritten in Monkey C
for the **Garmin Venu X1** (448×486 AMOLED, Connect IQ API 6). Textured walls,
variable floor and ceiling heights, sliding doors, enemies with the same
state-machine AI, dodgeable projectiles, pickups, status bar. All art is still
generated procedurally at start-up — there are no image resources.

Measured on the watch: **20 fps standing still, ~15.6 fps average moving**
(the straight port ran at 1–2). It is a plain watch-app with no permissions
and no background service, so it costs no battery unless it is open.

Quick start, if you already have the Connect IQ SDK Manager and libmtp:

```sh
./install.sh        # build, wait for the watch on USB, push it
```

Everything below explains the pieces.

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
  bitmaps), drawn at exactly the ray's width: no texel loop, no clip for a
  full-height face, and the GPU scales 32 pixels rather than a whole texture
  that is then clipped away. Step and beam faces of 4, 8 or 12 texels use
  exact-height columns (768 more) so they need no clip either; faces under
  6 px tall are one flat fill in the texture's average colour. (The first
  version drew the full 64-column texture clipped to the ray; the GPU work
  for that showed up as a mystery 10–30 ms charged to whatever call came
  next.)
- **Every sprite is one prebuilt palette bitmap per frame**, cropped to its
  opaque box, shaded with `setPalette`, drawn once per run of rays not hidden
  by a wall.
- **The DDA compares a precomputed per-cell key**: the common "nothing changed"
  step is one byte read and one compare. The solid map border replaces the
  bounds checks.
- **Rays are 16 px wide when standing still, 20 px while moving or turning.**
- **The wall pass is cached**: once the view has held still for three frames
  it is rendered into an offscreen bitmap a ray or two per frame, only while
  the frame has time to spare, and then blitted with one call per frame until
  the view changes. Standing still costs ~12 calls a frame. (Rendering *into*
  the offscreen bitmap is software and ~3× slower per call, which is why it
  is never done while moving.)
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
- **Start-up is ~140 small tasks**, a few per tick, so nothing trips the
  watchdog; the loading bar covers the 10–20 s it takes on the watch (most of
  it building ~1,300 tiny bitmaps).
- **One frame is one `onUpdate`**: the timer only requests redraws. Logic
  and rendering share the callback, and the loops are small enough that the
  watchdog is not a concern once start-up is done.

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

You need a JDK, the Connect IQ SDK, and the Venu X1 device definition. The
SDK Manager provides the last two (it needs a Garmin account to download):

```sh
brew install --cask temurin connectiq-sdk-manager
open -a SdkManager      # sign in; download an SDK and, under Devices, "Venu X1"
```

`build.sh` uses whichever SDK the manager marked current (that keeps the
compiler, simulator and device files from the same SDK) and generates a
developer signing key on first run:

```sh
./build.sh              # → bin/DoomCE-venux1.prg
./run.sh                # build, restart the simulator, push, show its console for 25 s
```

`run.sh` restarts the simulator every time because `monkeydo` hangs silently
if an app is already running in it, and it launches the SDK's own copy of the
simulator: the one the `connectiq` Homebrew cask puts in `/Applications`
cannot find the SDK's `version.txt` and shows an error dialog on every launch.
(That cask is not needed at all if you use the SDK Manager.)

## Install on the watch

The Venu X1 is MTP-only over USB, so it does not mount as a drive on macOS,
and libmtp's stock `mtp-sendfile` fails on it (it never sets a storage id).
`tools/mtp_push.c` is a small libmtp program that does it right:

```sh
brew install libmtp
./install.sh            # builds the app and tools/mtp_push, waits for the watch, pushes
```

or by hand:

```sh
make -C tools
tools/mtp_push bin/DoomCE-venux1.prg DoomCE.prg          # into GARMIN/Apps, replacing any old copy
```

Plug the watch in, unlocked; it can take 10–20 s to appear on the bus, and if
it charges but never appears, re-seat the clip. Then unplug and DOOMCE is in
the app list.

**Developer mode must be on** or the watch will not run a sideloaded app:
Settings → System → About, tap the serial number seven times; a *Developer
Mode* entry then appears at the bottom of that page.

A GUI alternative is OpenMTP (`brew install --cask openmtp`): drag the `.prg`
into `GARMIN/Apps`.

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

The watch appends `System.println` output to `GARMIN/Apps/LOGS/DoomCE.TXT`,
but only if that file already exists. Create it once, build with
`PROFILE = true` (top of `Engine.mc`), play, then pull the log:

```sh
: > /tmp/DoomCE.TXT && tools/mtp_push /tmp/DoomCE.TXT DoomCE.TXT GARMIN/Apps/LOGS
# ... build with PROFILE = true, install, play ...
tools/pull_log.sh       # saves the log and summarises still vs moving fps
```

A profile line every 32 frames gives fps, draw calls, cached frames and
per-phase milliseconds (walls / things+weapon / HUD / tick); the same build
shows fps and call count in the HUD. `BENCH = true` runs the micro-benchmarks
behind the cost table above at start-up. The log grows across runs; delete it
on the watch (`mtp-delfile -n <id>`) to start fresh.

## Publishing to the Connect IQ store

```sh
./release.sh            # → bin/DoomCE.iq, a release build for every product in the manifest
```

Upload that file at <https://apps.garmin.com/developer/> (Add an App; needs
the same Garmin account as the SDK Manager, plus acceptance of the developer
agreement). Every text field the form asks for — description, category,
support contact, privacy statement — is pre-written in
[store/listing.md](store/listing.md), and [store/screenshots/](store/screenshots/)
holds 448×486 captures from the simulator (File → Save Screen Capture). Read
the note on the app's name in that file before you submit.

## Layout

```
manifest.xml            watch-app, venux1 only, no permissions
monkey.jungle
source/DoomCEApp.mc     entry point
source/DoomView.mc      timer, onUpdate, touch/button delegate
source/Engine.mc        the game: tables, art, world, renderer, AI, frame loop
resources/              app name, launcher icon, title logo
build.sh                compile with the SDK Manager's current SDK
run.sh                  build + simulator
install.sh              build + push to the watch over MTP
tools/mtp_push.c        libmtp pusher (make -C tools)
tools/pull_log.sh       fetch and summarise the on-device profile log
tools/make_art.py       regenerates the icon and logo PNGs
```

Knobs, all at the top of `Engine.mc` / `DoomView.mc`: `TICK_MS` (frame
cadence; 100 halves the battery cost of playing), `XSTEP_STILL` /
`XSTEP_MOVE` (ray width), `TAP_TURN` (degrees per edge tap), `PROFILE`,
`BENCH`.
