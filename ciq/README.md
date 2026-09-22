# DOOMCE for Garmin Connect IQ

The same raycaster as [`../src/main.c`](../src/main.c), rewritten in Monkey C
for the **Garmin Venu X1** (448×486 AMOLED, Connect IQ API 6). Textured walls,
variable floor and ceiling heights, sliding doors, enemies with the same
state-machine AI, dodgeable projectiles, pickups, status bar. All art is still
generated procedurally at start-up — there are no image resources.

Verified in the Connect IQ simulator: 20 fps (timer-bound), ~400–1000
`fillRectangle` calls per frame, no watchdog trips.

## How the port works

The CE engine never writes pixels; it writes vertical runs of one colour, 8 px
wide (`fill_col8`). That is exactly `dc.fillRectangle`, so the algorithm carries
over unchanged. What changes is the cost model:

| | TI-84 Plus CE | Connect IQ |
|---|---|---|
| bound by | bytes painted (122 cycles/byte) | **loop iterations per callback** |
| hard limit | none — just slow | **watchdog kills the callback** |
| `*`, `>>`, `&` | library calls on 24-bit ints | native 32-bit |

The Connect IQ watchdog is the whole design constraint. Measured in the
simulator: a single callback dies somewhere between **8k and 16k** simple loop
iterations. So:

- **Start-up is 55 tasks, one per frame** (`buildTasks`). Palette, cos table,
  each texture strip, each sprite pass, each weapon bitmap — nothing runs long
  enough to trip it. There is a progress bar while it happens (~3 s).
- **Game logic runs in the timer callback, rendering in `onUpdate`** — two
  callbacks, two budgets.
- **Wall and sprite loops walk texels, not rows**, and never more texels than
  half the rows they cover (`ks` in `wallSlice` / `drawBillboard`). A 400-row
  wall face costs ≤32 iterations; a far one costs a handful.
- **Sprites are capped at ~14 columns** whatever their size, and a per-frame
  budget (`ITER_BUDGET`) drops the farthest ones first when a crowd is on screen.
- **The rifle is prerendered** into 9 small palette `BufferedBitmap`s
  (3 frames × 3 palettes) and drawn with one `drawScaledBitmap`. (Do not call
  `clear()` on a fresh palette bitmap — that paints it black. It starts
  transparent.)

Per-frame rates (movement, enemy speed, fire rate, projectile speed, animation
tics) are rescaled from the CE's ~6 fps to ~20 fps so the game plays at the same
real-world pace.

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
is already running in it.

## Install on the watch

Connect the watch over USB and copy `bin/DoomCE-venux1.prg` into
`GARMIN/Apps/`. It appears in the activity/app list as DOOMCE. No store
submission needed.

## Controls (Venu X1: two buttons + touch)

| Input | Action |
|---|---|
| tap upper centre / swipe up | forward |
| tap lower centre / swipe down | back |
| tap left / right edge | turn |
| swipe left / right | strafe |
| tap centre | open the door you are facing |
| tap the status bar / top button | fire |
| long-press any move zone | keep moving |
| bottom button | quit |
| any tap after death | restart |

Taps accumulate, so mashing forward walks further.

## Profiling

Set `PROFILE = true` in `Engine.mc`: every 8 frames the console prints fps,
draw calls, and wall/sprite loop iterations. Iterations are what the watchdog
counts; keep the sum comfortably under ~6000 per frame.

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
