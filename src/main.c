/* ------------------------------------------------------------------
 * DOOMCE - a first-person raycaster for the TI-84 Plus CE
 *
 * Copyright (C) 2026 happyc0der
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * All artwork is generated procedurally by this source at startup.
 * No assets from any third-party game are used or required.
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------
 * DOOMCE - a from-scratch raycaster for the TI-84 Plus CE
 *
 * Everything is 24-bit integer math (the eZ80's native `int` width in
 * the CE toolchain).  No floats are used after start-up.
 *
 * World units: 1.0 cell == 256 (8.8 fixed point, FIX = 8).
 * Angles:      0 .. NA-1 covers a full turn (power of two -> mask, no %).
 * ------------------------------------------------------------------ */

#include <graphx.h>
#include <keypadc.h>
#include <tice.h>

#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ---------- configuration ---------------------------------------- */

#define SCR_W       320
#define SCR_H       240
#define VIEW_H      200                 /* 3D viewport; rest is HUD.
                                         * 28% fewer rows than 200 = 28%
                                         * less fill, the dominant cost. */
#define VIEW_CY     (VIEW_H / 2)        /* horizon line                */

#define FIX         8
#define ONE         (1 << FIX)          /* 1.0 == one map cell         */

#define NA          2048                /* angle units in a full turn  */
#define NA_MASK     (NA - 1)
#define NA_90       (NA / 4)

#define PROJD       277                 /* (SCR_W/2) / tan(FOV/2), FOV=60 */
#define XSTEP       8                   /* cast every Nth column, widen to fill.
                                         * Walls are ~94% of frame time and cost
                                         * scales with ray count, so 2 ~= 2x fps. */
#define DIST_MIN    24                  /* clamp, ~0.09 cell           */
#define DIST_MAX    16383

#define MAPW        24
#define MAPH        24

#define TEXW        64                  /* wall texture width          */
#define TEXH        32                  /* height; tiles vertically    */
#define NWALLTEX    4


/* two 320x240x8bpp buffers live back-to-back at the top of RAM.
 * (the host test harness overrides these) */
#ifndef BUF_A
#define BUF_A       ((uint8_t *)0xD40000)
#define BUF_B       ((uint8_t *)0xD52C00)
#endif

/* ---------- tables ------------------------------------------------ */

static int16_t  cosTab[NA];    /* cos(a) * 256, fits a short            */
static uint16_t icosTab[NA];   /* 256 / |cos(a)|, clamped to DIST_MAX+1 */

#define SIN(a)   cosTab [((a) - NA_90) & NA_MASK]

static int projd     = PROJD;        /* tunable FOV: (SCR_W/2)/tan(fov/2) */
static int projScale = PROJD * ONE;  /* == projd*ONE, hoisted out of the hot loop */
static int moveSpeed = 14;           /* 8.8 cells per frame  */
static int turnSpeed = 26;           /* angle units per frame */

static int     colAng[SCR_W];  /* ray angle offset for screen column x */
static int     colCos[SCR_W];  /* cos of that offset * 256 (fisheye fix) */
static uint8_t bgCol[VIEW_H];  /* ceiling / floor gradient             */
static uint8_t shade [8][256]; /* shade[level][color] -> darker color   */
static uint8_t shadeE[8][256]; /* same, with field grey remapped to grey */
static uint16_t shadePair[8][256]; /* shade[l][c] doubled into a 16-bit pair.
                                    * One lookup per pixel instead of a lut
                                    * read + 24-bit shift + or, each of which
                                    * is a library CALL on eZ80. */

static uint8_t wallTex[NWALLTEX][TEXW][TEXH]; /* [tex][x][y] column-major */
static uint8_t *wallTexBase[NWALLTEX];        /* base ptr: no 3D index maths */
static unsigned rep3[256];                    /* colour -> that byte x3.
                                               * c * 0x010101 is a __imulu
                                               * CALL, once per fill run. */

static int zbuf[SCR_W];        /* per-column wall distance for sprites  */
static int atanTab[65];        /* atan(i/64) in angle units, 0 .. NA/8   */

/* ---------- world ------------------------------------------------- */

static const char mapSrc[MAPH][MAPW + 1] = {
    "111111111111111111111111",
    "1......1........2......1",
    "1......1........2......1",
    "1..33..1..2222..2..44..1",
    "1..3...1.....2.....4...1",
    "1..3...........2...4...1",
    "1..3333..1112..2..4444.1",
    "1............2.........1",
    "1....2222....2....3333.1",
    "1....2.......2.......3.1",
    "1....2..111111111....3.1",
    "1.......1.......1......1",
    "1.......1...44..1......1",
    "1..222..1...4...1..11..1",
    "1....2..1...4...1...1..1",
    "1....2..111.4.111...1..1",
    "1....2..............1..1",
    "1.......3333333.....1..1",
    "1..44...3.....3........1",
    "1...4...3..2..3...333..1",
    "1...4......2......3....1",
    "1..444.....2......333..1",
    "1..................3...1",
    "111111111111111111111111",
};
static uint8_t world[MAPH][MAPW];

/* Per-cell heights.  '.' is the default (floor 0, ceiling 1.0 cell).
 * '1'..'5' raise the floor in 1/8-cell steps; 'a'..'e' lower the ceiling.
 * 'D' is a door: a cell whose ceiling slides down to the floor when shut. */
static const char hSrc[MAPH][MAPW + 1] = {
    "........................",
    ".........12.bbb...12....",
    "....bb...bbbbbb.........",
    "........................",
    "........................",
    "........................",
    "......................D.",
    ".111.D...111............",
    ".131.....131............",
    ".111.....111.....111....",
    ".................131....",
    "............D....111....",
    "........................",
    "........................",
    "..111...................",
    "..131...................",
    "..111...D...............",
    "........................",
    "........................",
    "........................",
    "........................",
    "........................",
    "........................",
    "........................",
};

#define EYE_H   (ONE * 5 / 8)       /* eye above whatever floor you're on */
#define MAXSTEP (ONE * 3 / 8)       /* highest ledge you can walk up      */
#define HEADROOM (ONE / 2)          /* clearance needed to enter a cell   */

static uint8_t hmap[MAPH][MAPW];    /* hi nibble floor, lo nibble ceiling */

/* world[my][mx] compiles to my*MAPW+mx - a __imulu CALL - and the DDA
 * does that three times per step.  Row pointers turn it into a load. */
static uint8_t *worldRow[MAPH];
static uint8_t *hmapRow[MAPH];
static uint8_t *doorRow[MAPH];

#define MAXDOOR 8
typedef struct {
    uint8_t mx, my;
    uint8_t open;                   /* 0 = shut, 64 = fully open          */
    uint8_t want;                   /* target openness                    */
    uint8_t hold;                   /* frames left before auto-close      */
} door_t;
static door_t  doors[MAXDOOR];
static int     ndoor;
static uint8_t doorIdx[MAPH][MAPW]; /* door index + 1, 0 = not a door     */

static int eyeZ;                    /* recomputed every frame             */

static int floorz(int mx, int my)
{
    /* (h>>4)*32 == (h & 0xF0) << 1 : no multiply, no wide shift */
    return (int)(hmapRow[my][mx] & 0xF0) << 1;
}

static int ceilz(int mx, int my)
{
    uint8_t h    = hmapRow[my][mx];
    int     base = (int)(h & 15) << 5;          /* *32 */
    int     d    = doorRow[my][mx];
    if (d) {
        int f = (int)(h & 0xF0) << 1;
        return f + ((base - f) * doors[d - 1].open) / 64;
    }
    return base;
}

/* ---------- actors ------------------------------------------------ */

#define SPR_W   28
#define SPR_H   36
#define SPR_SZ  (SPR_W * SPR_H)

#define NROT    5              /* 5 drawn rotations, mirrored -> 8 views */
#define F_WALK  0              /* 5 rot x 2 frames                       */
#define F_FIRE  10             /* 5 rot                                  */
#define F_DIE   15             /* 5 frames, rotation independent         */
#define NFRAME  20
#define NETYPE  1   /* variant 2 is a ramp remap, not stored art */

#define MAXACT  20

enum { A_IDLE, A_CHASE, A_FIRE, A_PAIN, A_DIE, A_DEAD, A_GONE };
#define CORPSE_LIFE 120        /* frames a body lingers before it fades */

typedef struct {
    int     x, y;          /* 8.8 fixed position                  */
    int     dir;           /* facing, 0..NA-1                     */
    int     dist;          /* scratch, camera-space depth         */
    int16_t hp;
    uint8_t type, st, frame, tics, atk;
    int8_t  wob;           /* wander offset, in NA/16 units       */
} actor_t;

static actor_t act[MAXACT];
static int     nact;

static uint8_t eGfx[NETYPE * NFRAME][SPR_SZ];

/* ---------- pickups ------------------------------------------------ */

#define MAXPICK 14
#define PK_W 20
#define PK_H 20
enum { P_HEALTH, P_AMMO };

typedef struct { int x, y; uint8_t type, on; int dist; } pick_t;
static pick_t  picks[MAXPICK];
static int     npick;
static uint8_t pkGfx[2][PK_W * PK_H];

/* ---------- projectiles -------------------------------------------- */

#define MAXPROJ 12
#define PJ_W 12
#define PJ_H 12

typedef struct { int x, y, vx, vy, z; uint8_t on, life, frame; int dist; } proj_t;
static proj_t  projs[MAXPROJ];
static uint8_t pjGfx[2][PJ_W * PJ_H];

/* ---------- draw list ---------------------------------------------- */

enum { K_ACT, K_PICK, K_PROJ };
typedef struct { int dist; uint8_t kind, idx; } rend_t;
static rend_t rlist[MAXACT + MAXPICK + MAXPROJ];

/* ---------- weapon ------------------------------------------------ */

#define WPN_W 48
#define WPN_H 36
#define NWPN  3

static uint8_t wGfx[NWPN][WPN_W * WPN_H];
/* first/last non-transparent row per column: most of the gun canvas is
 * empty, and skipping it beats testing every pixel for transparency. */
static uint8_t wpnTop[NWPN][WPN_W], wpnBot[NWPN][WPN_W];
static int     wpnTic, wpnFrame, bobPhase, bobX, bobY;
static int     flashTic, painTic;

/* ---------- palettes ---------------------------------------------- */

static uint16_t palNormal[256], palPain[256], palFlash[256];
static int      curPal = -1;

/* ---------- player ------------------------------------------------ */

static int px, py;      /* 8.8 fixed */
static int pa;          /* 0 .. NA-1 */
static int health = 100, ammo = 50, kills;
static unsigned curFps;
/* The HUD sits below the 3D view, so the renderer never overwrites it.
 * Redrawing it costs 12,800 byte-writes ~= 17% of the frame, for nothing.
 * Redraw only when a value changes - twice, once per back buffer. */
static uint8_t hudDirty = 2;
static int hudH = -1, hudA = -1, hudK = -1;
static unsigned hudF = 0xFFFFFF;        /* measured, shown in the HUD */
static int viewCy = VIEW_CY;   /* horizon; drops when the player dies */
static int deadTic;

static uint8_t *back;   /* current draw buffer */

/* ------------------------------------------------------------------
 * start-up: palette, tables, textures
 * ------------------------------------------------------------------ */

/* 8 colour ramps x 32 brightness steps = 256 palette entries.
 * A colour byte is   (ramp << 5) | brightness   which makes shading a
 * single subtract, and makes index 0 pure black == sprite transparency. */
static const uint8_t rampRGB[8][3] = {
    { 200, 200, 200 },   /* 0 stone / grey   */
    { 195,  85,  65 },   /* 1 brick          */
    { 150, 105,  55 },   /* 2 wood / floor   */
    { 116, 124,  92 },   /* 3 field grey     */
    { 110, 126, 150 },   /* 4 steel          */
    { 225,  50,  50 },   /* 5 red            */
    { 235, 205,  85 },   /* 6 yellow         */
    { 225, 170, 140 },   /* 7 flesh          */
};

#define C(ramp, s)  (uint8_t)(((ramp) << 5) | (s))

static void init_palette(void)
{
    for (int r = 0; r < 8; r++)
        for (int s = 0; s < 32; s++)
            gfx_palette[(r << 5) | s] = gfx_RGBTo1555(rampRGB[r][0] * s / 31,
                                                      rampRGB[r][1] * s / 31,
                                                      rampRGB[r][2] * s / 31);

    for (int i = 0; i < 256; i++) palNormal[i] = gfx_palette[i];

    /* damage: push everything toward red. muzzle flash: lift everything. */
    for (int r = 0; r < 8; r++)
        for (int s2 = 0; s2 < 32; s2++) {
            int i = (r << 5) | s2;
            int R = rampRGB[r][0] * s2 / 31;
            int G = rampRGB[r][1] * s2 / 31;
            int B = rampRGB[r][2] * s2 / 31;
            int pr = R + (255 - R) * 3 / 5;
            palPain[i]  = gfx_RGBTo1555(pr, G / 3, B / 3);
            palFlash[i] = gfx_RGBTo1555(R + (255 - R) / 3,
                                        G + (255 - G) / 3,
                                        B + (255 - B) / 4);
        }

    for (int l = 0; l < 8; l++)
        for (int c = 0; c < 256; c++) {
            int r = c >> 5;
            /* scale, don't subtract: subtracting a constant crushes every
             * colour to black at the same range and makes mid-distance
             * enemies invisible.  Scaling keeps relative contrast. */
            int v = ((c & 31) * (16 - l * 2)) >> 4;
            if (v > 31) v = 31;
            shade [l][c] = (uint8_t)((r << 5) | v);
            shadeE[l][c] = (uint8_t)(((r == 3 ? 0 : r) << 5) | v);
            shadePair[l][c] = (uint16_t)shade[l][c] * 0x0101u;
        }
}

static void init_tables(void)
{
    const float k = 2.0f * 3.14159265f / (float)NA;

    projScale = projd * ONE;

    for (int i = 0; i < NA; i++) {
        float c  = cosf((float)i * k);
        float ac = fabsf(c);
        int   v;

        cosTab[i] = (int16_t)(int)(c * 256.0f + (c < 0 ? -0.5f : 0.5f));
        v = (ac < 0.00002f) ? DIST_MAX + 1 : (int)(256.0f / ac);
        icosTab[i] = (uint16_t)(v > DIST_MAX + 1 ? DIST_MAX + 1 : v);
    }

    for (int x = 0; x < SCR_W; x++) {
        float t = atanf(((float)x - (float)(SCR_W / 2) + 0.5f) / (float)projd);
        float u = t / k;
        colAng[x] = (int)(u + (u < 0 ? -0.5f : 0.5f));
        colCos[x] = (int)(cosf(t) * 256.0f + 0.5f);
    }

    for (int i = 0; i <= 64; i++) {
        float t = atanf((float)i / 64.0f) / k;
        atanTab[i] = (int)(t + 0.5f);
    }

    for (int y = 0; y < VIEW_CY; y++)           /* ceiling: dark blue-grey */
        bgCol[y] = C(4, 2 + (VIEW_CY - y) * 9 / VIEW_CY);
    for (int y = VIEW_CY; y < VIEW_H; y++)      /* floor: brown gradient   */
        bgCol[y] = C(2, 3 + (y - VIEW_CY) * 13 / (VIEW_H - VIEW_CY));
}

static unsigned rng_state = 0x1234ABu;
static unsigned rnd(void)
{
    rng_state ^= rng_state << 5;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 3;
    return rng_state & 0xFFFFFFu;
}

static void init_textures(void)
{
    for (int i = 0; i < NWALLTEX; i++) wallTexBase[i] = &wallTex[i][0][0];
    for (int i = 0; i < 256; i++) rep3[i] = (unsigned)i * 0x010101u;

    for (int x = 0; x < TEXW; x++)
        for (int y = 0; y < TEXH; y++) {
            int n = (int)(rnd() & 3);

            /* 1: brick */
            {
                int row = y >> 4;
                int bx  = (x + ((row & 1) ? 16 : 0)) & 31;
                wallTex[0][x][y] = ((y & 15) < 2 || bx < 2)
                                 ? C(0, 11 + n)
                                 : C(1, 19 + n);
            }
            /* 2: rough stone */
            wallTex[1][x][y] = C(0, 16 + n + (((x ^ y) & 8) ? 3 : 0));

            /* 3: wood planks with vertical grain */
            wallTex[2][x][y] = ((x & 15) == 0)
                             ? C(2, 7)
                             : C(2, 18 + n + (((x * 5) & 7) >> 1));

            /* 4: tech panel */
            {
                int e = (x & 31) < 2 || (y & 31) < 2;
                int lamp = ((x & 31) > 12 && (x & 31) < 20 &&
                            (y & 31) > 12 && (y & 31) < 20);
                wallTex[3][x][y] = lamp ? C(6, 26 + n)
                                 : e    ? C(0, 8)
                                        : C(0, 15 + n);
            }

        }
}


/* ------------------------------------------------------------------
 * angle helpers
 * ------------------------------------------------------------------ */

static bool solid_at(int fx, int fy);
static void spawn_proj(actor_t *a);

#define PROJ_SPD 30        /* ~0.12 cell per frame: slow enough to dodge */

static int point_angle(int dx, int dy)
{
    int ax = dx < 0 ? -dx : dx;
    int ay = dy < 0 ? -dy : dy;
    int a;

    if (!ax && !ay) return 0;
    if (ax >= ay) a = atanTab[(ay << 6) / ax];
    else          a = NA_90 - atanTab[(ax << 6) / ay];
    if (dx < 0)   a = (NA / 2) - a;
    if (dy < 0)   a = -a;
    return a & NA_MASK;
}

/* coarse line of sight: step ~1/4 cell and test for walls */
static bool los(int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0, dy = y1 - y0;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    int n  = (ax > ay ? ax : ay) >> 6;
    int sx, sy, x = x0, y = y0;

    if (n < 2) return true;
    if (n > 72) n = 72;
    sx = dx / n; sy = dy / n;
    for (int i = 1; i < n; i++) {
        x += sx; y += sy;
        if (solid_at(x, y)) return false;
    }
    return true;
}

/* ------------------------------------------------------------------
 * sprite painting helpers  (frames are column-major, like the walls)
 * ------------------------------------------------------------------ */

static uint8_t *gspr;
static int      gw, gh;
static int      gexp;          /* expand every shape by N px (outline pass) */
static uint8_t  gforce;        /* non-zero: paint everything this colour     */

static void gpx(int x, int y, uint8_t c)
{
    if ((unsigned)x < (unsigned)gw && (unsigned)y < (unsigned)gh)
        gspr[x * gh + y] = c;
}

static void grect(int x0, int y0, int x1, int y1, uint8_t c)
{
    if (gforce) c = gforce;
    for (int x = x0 - gexp; x <= x1 + gexp; x++)
        for (int y = y0 - gexp; y <= y1 + gexp; y++) gpx(x, y, c);
}

/* ellipse lit from the upper left, so limbs and torsos read as round */
static void gell(int cx, int cy, int rx, int ry, int ramp, int base)
{
    rx += gexp; ry += gexp;
    if (rx < 1) rx = 1;
    if (ry < 1) ry = 1;
    for (int x = cx - rx; x <= cx + rx; x++)
        for (int y = cy - ry; y <= cy + ry; y++) {
            int ux = ((x - cx) << 8) / rx;
            int uy = ((y - cy) << 8) / ry;
            int l;
            if (ux * ux + uy * uy > 65536) continue;
            if (gforce) { gpx(x, y, gforce); continue; }
            l = base + ((-ux - uy) >> 6);
            if (l < 2)  l = 2;
            if (l > 31) l = 31;
            gpx(x, y, C(ramp, l));
        }
}

/* ------------------------------------------------------------------
 * enemy artwork: generic WW2-era infantry -- field-grey tunic,
 * coal-scuttle helmet, bolt-action rifle.  Built from primitives, so
 * there is no asset pipeline to feed and nothing to store on disk.
 *
 * Every figure is painted twice: once expanded in near-black, then
 * normally on top.  That gives a 1px outline for free and is what
 * makes the sprite read against a dark wall.
 * ------------------------------------------------------------------ */

static void soldier_body(int rot, int legph, int firing)
{
    const int uni = 3;                 /* field grey (remapped per type) */
    const int ub  = 21;
    /* torso half-width per rotation: a profile is much narrower than a
     * front or back view.  This is what actually sells the facing. */
    static const int8_t trx[NROT]  = { 6, 6, 4, 5, 6 };
    static const int8_t lean[NROT] = { 0, 1, 2, 1, 0 };
    int cx   = 14 + lean[rot];
    int rx   = trx[rot];
    int back = (rot >= 3);
    int side = (rot == 2);

    /* legs */
    for (int i = 0; i < 2; i++) {
        int lx  = cx + (i ? (side ? 2 : 4) : (side ? -2 : -4));
        int ph  = (legph ^ i) & 1;
        int bot = 31 - (ph ? 2 : 0);
        grect(lx - 2, 23, lx + 1, bot, C(uni, ub - 6));
        grect(lx - 2, bot + 1, lx + 1, 35, C(2, 7));
    }

    /* torso */
    gell(cx, 18, rx, 7, uni, ub);

    /* rucksack + rolled blanket, only from behind */
    if (back) {
        gell(cx, 17, rx - 1, 5, 2, 11);
        grect(cx - rx + 1, 13, cx + rx - 1, 14, C(2, 14));
    }

    /* arms */
    if (side) {
        grect(cx + 2, 14, cx + 4, 23, C(uni, ub - 7));
    } else {
        grect(cx - rx - 2, 14, cx - rx, 23, C(uni, ub - 7));
        grect(cx + rx,     14, cx + rx + 2, 23, C(uni, ub - 7));
    }
    grect(cx - rx, 22, cx + rx, 23, C(2, 5));                 /* belt */

    /* rifle */
    if (firing) {
        if (back) {                                    /* seen from behind */
            grect(cx - 3, 15, cx + 3, 17, C(uni, ub - 4));
            grect(cx - 1, 13, cx + 1, 15, C(4, 15));
        } else {
            grect(cx - 4, 15, cx + 7, 17, C(uni, ub - 4));
            grect(cx + 1, 13, 27, 14, C(2, 9));
            grect(cx + 6, 12, 27, 12, C(4, 17));
        }
    } else if (side) {
        grect(cx + 1, 13, 26, 14, C(2, 9));                   /* carried  */
        grect(cx + 5, 12, 26, 12, C(4, 15));
    } else if (!back) {
        grect(cx + rx + 1, 9, cx + rx + 2, 26, C(2, 8));      /* slung    */
        grect(cx + rx,    11, cx + rx + 3, 12, C(4, 14));
    }

    /* head */
    if (back) {
        gell(cx, 12, 4, 3, uni, ub - 8);                      /* collar   */
    } else if (side) {
        gell(cx + 1, 12, 3, 3, 7, 24);
        gpx(cx + 4, 12, C(7, 27));                            /* nose     */
        gpx(cx + 2, 12, C(0, 3));                             /* one eye  */
    } else {
        gell(cx, 12, 4, 3, 7, 24);
        if (rot == 0) { gpx(cx - 2, 12, C(0, 3)); gpx(cx + 2, 12, C(0, 3)); }
        else          { gpx(cx - 1, 12, C(0, 3)); gpx(cx + 3, 12, C(0, 3)); }
    }

    /* helmet: dome plus the flared rim that gives the silhouette */
    gell(cx, 7, side ? 5 : 6, 4, 0, back ? 16 : 19);
    grect(cx - (side ? 5 : 7), 9, cx + (side ? 5 : 7), 10, C(0, back ? 12 : 15));
}

static void draw_soldier(int rot, int legph, int firing)
{
    memset(gspr, 0, SPR_SZ);
    gexp = 1; gforce = C(0, 2);  soldier_body(rot, legph, firing);
    gexp = 0; gforce = 0;        soldier_body(rot, legph, firing);
}

/* death: the body folds forward and goes down, helmet comes off at the end */
static void death_body(int f)
{
    const int uni = 3, ub = 21;

    if (f >= 3) gell(15, 34, 10 + f, 2, 5, 12);               /* pool   */

    switch (f) {
    case 0:                                                   /* staggered */
        grect(10, 24, 13, 35, C(uni, ub - 6));
        grect(15, 24, 18, 35, C(uni, ub - 6));
        gell(14, 19, 6, 7, uni, ub);
        gell(15, 12, 4, 3, 7, 22);
        gell(15,  8, 6, 4, 0, 18);
        grect(8, 10, 22, 11, C(0, 14));
        break;
    case 1:                                                   /* doubled over */
        grect(11, 26, 14, 35, C(uni, ub - 6));
        grect(16, 26, 19, 35, C(uni, ub - 6));
        gell(15, 22, 7, 6, uni, ub - 1);
        gell(19, 17, 4, 3, 7, 21);
        gell(20, 14, 6, 4, 0, 17);
        break;
    case 2:                                                   /* on knees */
        grect(9, 29, 20, 35, C(uni, ub - 7));
        gell(15, 26, 8, 5, uni, ub - 2);
        gell(21, 23, 4, 3, 7, 20);
        gell(22, 21, 5, 4, 0, 16);
        break;
    case 3:                                                   /* prone */
        gell(15, 31, 11, 4, uni, ub - 3);
        grect(5, 30, 10, 34, C(uni, ub - 6));
        gell(24, 30, 4, 3, 7, 19);
        gell(7, 28, 4, 3, 0, 15);                             /* helmet off */
        break;
    default:                                                  /* flat */
        gell(15, 33, 12, 3, uni, ub - 5);
        grect(4, 32, 9, 35, C(uni, ub - 8));
        gell(25, 33, 4, 2, 7, 18);
        gell(6, 30, 4, 2, 0, 14);
        break;
    }
}

static void draw_death(int f)
{
    memset(gspr, 0, SPR_SZ);
    gexp = 1; gforce = C(0, 2);  death_body(f);
    gexp = 0; gforce = 0;        death_body(f);
}

/* ------------------------------------------------------------------
 * the player's rifle, three frames, drawn 2x into the viewport
 * ------------------------------------------------------------------ */

static void gun_body(int f)
{
    int lift = (f == 1) ? 4 : (f == 2) ? 2 : 0;    /* recoil */

    grect(10, 27 - lift, 33, 35,      C(2, 15));   /* butt / stock     */
    grect(14, 23 - lift, 31, 28 - lift, C(2, 18));
    grect(18, 15 - lift, 30, 25 - lift, C(0, 17)); /* receiver         */
    grect(31, 19 - lift, 37, 22 - lift, C(0, 16)); /* bolt handle      */
    grect(22, 24 - lift, 28, 30 - lift, C(0, 10)); /* magazine         */
    grect(21,  2 - lift, 27, 17 - lift, C(4, 22)); /* barrel           */
    grect(20,  1 - lift, 28,  4 - lift, C(0, 15)); /* front sight band */
    grect(23,  0 - lift, 25,  3 - lift, C(0, 20)); /* blade            */

    gell(19, 26 - lift, 4, 3, 7, 25);              /* fore hand        */
    gell(28, 31 - lift, 4, 3, 7, 23);              /* trigger hand     */
}

static void draw_gun(int f)
{
    memset(gspr, 0, WPN_W * WPN_H);
    gexp = 1; gforce = C(0, 2);  gun_body(f);
    gexp = 0; gforce = 0;        gun_body(f);

    if (f == 1) {                                   /* muzzle flash */
        gell(24, 3, 9, 6, 6, 31);
        gell(24, 2, 5, 4, 6, 31);
        grect(22, 0, 26, 9, C(6, 31));
        grect(15, 2, 33, 3, C(6, 27));
        grect(18, 0, 30, 1, C(6, 24));
    }
}

static void init_actor_gfx(void)
{
    gw = SPR_W; gh = SPR_H;
    for (int r = 0; r < NROT; r++)
        for (int f = 0; f < 2; f++) {
            gspr = eGfx[F_WALK + r * 2 + f];
            draw_soldier(r, f, 0);
        }
    for (int r = 0; r < NROT; r++) {
        gspr = eGfx[F_FIRE + r];
        draw_soldier(r, 0, 1);
    }
    for (int f = 0; f < 5; f++) {
        gspr = eGfx[F_DIE + f];
        draw_death(f);
    }
    gw = WPN_W; gh = WPN_H;
    for (int f = 0; f < NWPN; f++) { gspr = wGfx[f]; draw_gun(f); }
    for (int f = 0; f < NWPN; f++)
        for (int cx = 0; cx < WPN_W; cx++) {
            const uint8_t *cc = wGfx[f] + cx * WPN_H;
            int a = 0, b = WPN_H;
            while (a < WPN_H && !cc[a]) a++;
            while (b > a && !cc[b - 1]) b--;
            wpnTop[f][cx] = (uint8_t)a;
            wpnBot[f][cx] = (uint8_t)b;
        }

    /* pickups: a medical case and an ammunition box */
    gw = PK_W; gh = PK_H;
    gspr = pkGfx[P_HEALTH];
    memset(gspr, 0, PK_W * PK_H);
    gexp = 1; gforce = C(0, 2);
    grect(2, 5, 17, 18, 0); gexp = 0; gforce = 0;
    grect(2, 5, 17, 18, C(0, 26));                 /* white case   */
    grect(2, 5, 17, 7,  C(0, 20));                 /* lid          */
    grect(8, 9, 11, 16, C(5, 29));                 /* cross        */
    grect(5, 11, 14, 14, C(5, 29));
    grect(8, 3, 11, 5,  C(0, 14));                 /* handle       */

    gspr = pkGfx[P_AMMO];
    memset(gspr, 0, PK_W * PK_H);
    gexp = 1; gforce = C(0, 2);
    grect(2, 6, 17, 18, 0); gexp = 0; gforce = 0;
    grect(2, 6, 17, 18, C(2, 14));                 /* wooden crate */
    grect(2, 6, 17, 8,  C(2, 18));
    for (int i = 0; i < 4; i++)
        grect(4 + i * 4, 10, 6 + i * 4, 16, C(6, 22));  /* brass rounds */

    /* enemy projectile: a bright bolt with a hot core */
    gw = PJ_W; gh = PJ_H;
    for (int f = 0; f < 2; f++) {
        int r = f ? 5 : 4;
        gspr = pjGfx[f];
        memset(gspr, 0, PJ_W * PJ_H);
        gell(6, 6, r, r, 5, 22);
        gell(6, 6, r - 2, r - 2, 6, 31);
    }
}

static void init_world(void)
{
    for (int y = 0; y < MAPH; y++)
        for (int x = 0; x < MAPW; x++) {
            char c = mapSrc[y][x];
            world[y][x] = (c >= '1' && c <= '4') ? (uint8_t)(c - '0') : 0;
        }

    for (int y = 0; y < MAPH; y++) {
        worldRow[y] = world[y];
        hmapRow[y]  = hmap[y];
        doorRow[y]  = doorIdx[y];
    }

    ndoor = 0;
    memset(doorIdx, 0, sizeof doorIdx);
    for (int y = 0; y < MAPH; y++)
        for (int x = 0; x < MAPW; x++) {
            char h = hSrc[y][x];
            int fh = 0, ch = 8;
            if (h >= '1' && h <= '5') fh = h - '0';
            else if (h >= 'a' && h <= 'e') ch = 8 - (h - 'a' + 1);
            else if (h == 'D' && !world[y][x] && ndoor < MAXDOOR) {
                doors[ndoor].mx = (uint8_t)x; doors[ndoor].my = (uint8_t)y;
                doors[ndoor].open = 0; doors[ndoor].want = 0; doors[ndoor].hold = 0;
                doorIdx[y][x] = (uint8_t)(++ndoor);
            }
            hmap[y][x] = (uint8_t)((fh << 4) | ch);
        }

    px = 2 * ONE + ONE / 2;   /* cell (2,2): open, open on all sides */
    py = 2 * ONE + ONE / 2;
    pa = 0;

    static const uint8_t place[][3] = {
        { 12,  4, 0 },
        { 17,  8, 0 },
        {  6, 14, 0 },
        { 21, 19, 1 },
        { 11, 11, 0 },
        {  5, 19, 0 },
        {  8,  7, 1 },
        {  9,  8, 0 },
        { 15, 16, 0 },
        { 18,  3, 1 },
    };
    nact = (int)(sizeof place / sizeof place[0]);
    for (int i = 0; i < nact; i++) {
        actor_t *a = &act[i];
        a->x    = place[i][0] * ONE + ONE / 2;
        a->y    = place[i][1] * ONE + ONE / 2;
        a->type = place[i][2];
        a->hp   = a->type ? 60 : 30;
        a->st   = A_IDLE;
        a->dir  = (i * 349) & NA_MASK;
        a->frame = a->tics = a->atk = 0;
        a->wob  = 0;
    }
    {
        static const uint8_t pk[][3] = {
            {  4,  2, P_AMMO   }, { 20,  2, P_HEALTH },
            { 13,  7, P_AMMO   }, {  3, 11, P_HEALTH },
            { 21, 11, P_AMMO   }, { 12, 13, P_HEALTH },
            {  2, 17, P_AMMO   }, { 18, 17, P_HEALTH },
            {  6, 21, P_AMMO   }, { 15, 22, P_HEALTH },
        };
        npick = (int)(sizeof pk / sizeof pk[0]);
        for (int i = 0; i < npick; i++) {
            picks[i].x    = pk[i][0] * ONE + ONE / 2;
            picks[i].y    = pk[i][1] * ONE + ONE / 2;
            picks[i].type = pk[i][2];
            picks[i].on   = 1;
        }
    }
    for (int i = 0; i < MAXPROJ; i++) projs[i].on = 0;

    health = 100; ammo = 50; kills = 0;
    viewCy = VIEW_CY; deadTic = 0;
    wpnTic = wpnFrame = bobPhase = bobX = bobY = 0;
    flashTic = painTic = 0;
    hudDirty = 2; hudH = hudA = hudK = -1;
    eyeZ = floorz(px >> FIX, py >> FIX) + EYE_H;
}

/* ------------------------------------------------------------------
 * wall casting  -- the core loop
 * ------------------------------------------------------------------ */

/* Own rectangle fill, bounds-checked, straight into the back buffer.
 * graphx's gfx_FillRectangle was implicated in an NMI (write to address
 * 0) under the autotester, and the game needs none of graphx's drawing. */
static void fill_rect(int x0, int y0, int w, int h, uint8_t c)
{
    int x1 = x0 + w, y1 = y0 + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCR_W) x1 = SCR_W;
    if (y1 > SCR_H) y1 = SCR_H;
    if (y0 >= y1) return;
    {
        uint8_t *row = back + y0 * SCR_W + x0;   /* one multiply, not per row */
        int      n   = x1 - x0;
        for (int y = y0; y < y1; y++) {
            uint8_t *p = row;
            for (int i = 0; i < n; i++) *p++ = c;
            row += SCR_W;
        }
    }
}

__attribute__((noinline))
static uint8_t *fill_col8(uint8_t *p, uint8_t c, uint8_t run)
{
    unsigned ccc = rep3[c];                      /* lookup, not a multiply */
#ifndef __ez80__                                 /* host test build */
    { uint16_t cc = (uint16_t)c | ((uint16_t)c << 8);
      while (run--) {
          *(uint16_t *)p = cc; *(uint16_t *)(p+2) = cc;
          *(uint16_t *)(p+4) = cc; *(uint16_t *)(p+6) = cc;
          p += SCR_W;
      }
      (void)ccc; return p; }
#else
    __asm__ volatile (
        "   ld de, 313      \n"   /* 320 - the 7 we already advanced */
        "1: ld (hl), bc     \n"   /* bytes 0,1,2 */
        "   inc hl          \n"
        "   inc hl          \n"
        "   inc hl          \n"
        "   ld (hl), bc     \n"   /* bytes 3,4,5 */
        "   inc hl          \n"
        "   inc hl          \n"
        "   inc hl          \n"
        "   ld (hl), c      \n"   /* byte 6 */
        "   inc hl          \n"
        "   ld (hl), c      \n"   /* byte 7 */
        "   add hl, de      \n"   /* advance to the next row */
        "   dec a           \n"
        "   jr nz, 1b       \n"
        : "+l"(p), "+a"(run)
        : "c"(ccc)
        : "de", "memory");
    return p;             /* the loop already advanced HL by run * SCR_W */
#endif
}

static void vfill(uint8_t *fb, int x, int y0, int y1, uint8_t c)
{
    if (y0 < 0) y0 = 0;
    if (y1 > VIEW_H) y1 = VIEW_H;
    if (y0 >= y1) return;
    fill_col8(fb + y0 * SCR_W + x, c, (uint8_t)(y1 - y0));   /* eZ80 asm */
}

/* One textured wall face.  yTopF/yBotF are where the face's top and
 * bottom edges land unclipped; y0/y1 are the part we may actually draw.
 *
 * The inner loop steps the texture with a Bresenham DDA - adds and
 * compares only.  The obvious fixed-point version ((row * step) >> 16)
 * costs a __lmulu AND a __lshru library call per pixel on the eZ80,
 * which has no barrel shifter and no 32-bit multiply.  That was the
 * difference between 1 fps and something playable. */
/* Hand-written eZ80 for the hot fill: `run` rows of 8 identical pixels,
 * stride SCR_W.  All 8 bytes are the same colour, so BC holds it in all
 * three bytes and two 24-bit stores plus two byte stores cover the span.
 * The C version spilled to the stack every row; this keeps p, the colour
 * and the counter pinned in HL / BC / A.  Caller guarantees run >= 1. */
static void wall_slice(uint8_t *fb, int x, int y0, int y1,
                       int yTopF, int yBotF, int zTop, int zBot,
                       const uint8_t *tcol, const uint16_t *plut)
{
    int P = yBotF - yTopF;
    int texels = (zTop - zBot) >> 3;
    int rows, t, err;
    uint8_t *p;

    if (y0 < 0) y0 = 0;
    if (y1 > VIEW_H) y1 = VIEW_H;
    if (y0 >= y1 || P <= 0) return;
    if (texels < 1) texels = 1;

    rows = y1 - y0;
    { int skip = y0 - yTopF;                 /* one mul+div per slice */
      int n = skip * texels;
      t = n / P;
      err = n - t * P; }

    p = fb + y0 * SCR_W + x;

    /* Each texel covers a RUN of consecutive rows.  Splitting the walk
     * from the fill means neither loop needs many live values - the
     * eZ80 has 5 registers, and the combined loop spilled 12 times per
     * pixel.  The fill loop below holds only p, run and the colour. */
    while (rows > 0) {
        uint16_t cc = plut[tcol[t & (TEXH - 1)]];
        int run = 0;

        do { run++; err += texels; } while (err < P && run < rows);
        while (err >= P) { err -= P; t++; }
        rows -= run;

        if (run > 0) {                       /* the hot fill, in eZ80 asm */
            p = fill_col8(p, (uint8_t)cc, (uint8_t)run);
        }
    }
}

/* ------------------------------------------------------------------
 * wall casting.  Unlike a flat raycaster this does not stop at the
 * first wall: it keeps stepping outward, narrowing a per-column clip
 * window, drawing a face wherever the floor rises or the ceiling drops.
 * That is what gives steps, pits, low ceilings and sliding doors.
 * ------------------------------------------------------------------ */

static void render_walls(void)
{
    uint8_t *fb    = back;
    int      mapX0 = px >> FIX, mapY0 = py >> FIX;
    int      fracX = px & (ONE - 1), fracY = py & (ONE - 1);

    for (int x = 0; x < SCR_W; x += XSTEP) {
        /* (v & NA_MASK) is a __iand CALL on a 24-bit int.  The values are
         * already nearly in range, so a compare and an add do the same job. */
        int ang = pa + colAng[x];
        int sa;
        int cs, sn, ddx, ddy;
        if (ang < 0) ang += NA; else if (ang >= NA) ang -= NA;
        sa = ang - NA_90;
        if (sa < 0) sa += NA;
        cs = cosTab[ang];  sn  = cosTab[sa];
        ddx = icosTab[ang]; ddy = icosTab[sa];
        int mapX = mapX0, mapY = mapY0;
        int stepX, stepY, sdx, sdy, side = 0;
        int ytop = 0, ybot = VIEW_H;
        int pf = floorz(mapX0, mapY0);
        int pc = ceilz(mapX0, mapY0);
        int guard = 22;

        for (int k = 0; k < XSTEP; k++) zbuf[x + k] = DIST_MAX;

        if (cs < 0) { stepX = -1; sdx = (fracX * ddx) >> FIX; }
        else        { stepX =  1; sdx = ((ONE - fracX) * ddx) >> FIX; }
        if (sn < 0) { stepY = -1; sdy = (fracY * ddy) >> FIX; }
        else        { stepY =  1; sdy = ((ONE - fracY) * ddy) >> FIX; }

        while (guard--) {
            int dist, pdist, scale, lv, ypf, ypc;
            uint8_t wallx;
            int tid = 0, f = pf, c = pc, oob = 0;
            const uint16_t *lut;

            if (sdx < sdy) { sdx += ddx; mapX += stepX; side = 0; }
            else           { sdy += ddy; mapY += stepY; side = 1; }

            /* Decide whether this boundary changes anything BEFORE paying for
             * the projection. In a flat corridor the floor and ceiling are
             * unchanged for many cells, and the span we would draw is exactly
             * the one we will draw at the next real change - so skipping is
             * free visually and saves a 24-bit divide per boundary. */
            if ((unsigned)mapX >= MAPW || (unsigned)mapY >= MAPH) {
                oob = 1;
            } else {
                tid = worldRow[mapY][mapX];
                if (!tid) {
                    f = floorz(mapX, mapY);
                    c = ceilz(mapX, mapY);
                    if (f == pf && c == pc) continue;
                }
            }

            dist = (side == 0) ? sdx - ddx : sdy - ddy;
            if (dist < DIST_MIN) dist = DIST_MIN;
            if (dist > DIST_MAX) dist = DIST_MAX;
            pdist = (dist * colCos[x]) >> FIX;
            if (pdist < DIST_MIN) pdist = DIST_MIN;

            scale = projScale / pdist;
            lv    = pdist >> 10;
            if (side) lv += 1;
            if (lv > 7) lv = 7;
            lut = shadePair[lv];

            /* where the cell we are leaving projects at this boundary */
            ypf = viewCy + (((eyeZ - pf) * scale) >> FIX);
            ypc = viewCy + (((eyeZ - pc) * scale) >> FIX);

            /* its floor and ceiling spans, flat-shaded by distance */
            if (ypf < ybot) {
                int t = ypf > ytop ? ypf : ytop;
                vfill(fb, x, t, ybot, lut[C(2, 21)]);
                ybot = t;
            }
            if (ypc > ytop) {
                int t = ypc < ybot ? ypc : ybot;
                vfill(fb, x, ytop, t, lut[C(4, 13)]);
                ytop = t;
            }
            if (ytop >= ybot) break;
            if (oob) break;

            /* the texture column is the same for every face in this cell */
            if (side == 0) {
                wallx = (uint8_t)(py + ((dist * sn) >> FIX));   /* 8-bit wrap */
                if (stepX > 0) wallx = (uint8_t)(255 - wallx);
            } else {
                wallx = (uint8_t)(px + ((dist * cs) >> FIX));
                if (stepY < 0) wallx = (uint8_t)(255 - wallx);
            }

            if (tid) {                                   /* solid block */
                wall_slice(fb, x, ytop, ybot, ypc, ypf, pc, pf,
                           wallTexBase[tid - 1] + ((int)(wallx & 0xFC) << 3), lut);
                for (int k = 0; k < XSTEP; k++) zbuf[x + k] = pdist;
                ytop = ybot;
                break;
            }

            {
                if (f > pf) {                            /* floor steps up */
                    int yf = viewCy + (((eyeZ - f) * scale) >> FIX);
                    if (yf < ybot) {
                        int t = yf > ytop ? yf : ytop;
                        wall_slice(fb, x, t, ybot, yf, ypf, f, pf,
                                   wallTexBase[1] + ((int)(wallx & 0xFC) << 3), lut);
                        ybot = t;
                    }
                }
                if (c < pc) {                            /* ceiling drops */
                    int yc = viewCy + (((eyeZ - c) * scale) >> FIX);
                    if (yc > ytop) {
                        int t = yc < ybot ? yc : ybot;
                        wall_slice(fb, x, ytop, t, ypc, yc, pc, c,
                                   wallTexBase[2] + ((int)(wallx & 0xFC) << 3), lut);
                        ytop = t;
                    }
                }
                pf = f; pc = c;
            }
            if (ytop >= ybot) break;
        }

        if (ytop < ybot) {                               /* open sky / void */
            uint8_t *p = fb + (ytop < 0 ? 0 : ytop) * SCR_W + x;
            for (int y = ytop < 0 ? 0 : ytop; y < ybot && y < VIEW_H; y++) {
                uint8_t  bc = bgCol[y];
                uint16_t bb = (uint16_t)bc | ((uint16_t)bc << 8);
                *(uint16_t *)p       = bb;
                *(uint16_t *)(p + 2) = bb;
                *(uint16_t *)(p + 4) = bb;
                *(uint16_t *)(p + 6) = bb;
                p += SCR_W;
            }
        }
    }
}

static const uint8_t rotMap[8]  = { 0, 1, 2, 3, 4, 3, 2, 1 };
static const uint8_t rotMirr[8] = { 0, 0, 0, 0, 0, 1, 1, 1 };

/* ------------------------------------------------------------------
 * actors: billboarded, animated, clipped against the wall z-buffer
 * ------------------------------------------------------------------ */

/* One billboard standing at world height wz, worldH tall. Everything
 * that is not a wall goes through here, so raised floors just work. */
/* One billboard standing at world height wz, worldH tall.
 *
 * Same treatment the wall renderer got: Bresenham stepping instead of
 * fixed point, because (acc >> 16) is a __ishru library CALL per pixel
 * on the eZ80.  Columns are drawn 2 wide so the per-iteration cost -
 * which dominates - is halved. */
static void draw_billboard(int fwd, int rgt, int wz, int worldH,
                           const uint8_t *g, int gwid, int ghei,
                           const uint8_t *lut, int mirror)
{
    uint8_t *fb = back;
    int scale = projScale / fwd;
    int yb = viewCy + (((eyeZ - wz) * scale) >> FIX);
    int yt = viewCy + (((eyeZ - (wz + worldH)) * scale) >> FIX);
    int h  = yb - yt;
    int w, sx, x0, x1, xs, xe;
    int vstep, verr, tx, hacc, hstep, herr, xw;

    if (h < 2) return;
    w = (h * gwid) / ghei;
    if (w < 2) return;

    sx = SCR_W / 2 + (rgt * projd) / fwd;
    x0 = sx - (w >> 1); x1 = x0 + w;

    vstep = ghei / h;  verr = ghei - vstep * h;     /* per-column setup */
    hstep = gwid / w;  herr = gwid - hstep * w;

    /* A close sprite is magnified enormously - w can exceed the screen
     * width, so one body costs more than the entire level.  Widen the
     * column step with the magnification: at 20x there is no detail in
     * a 2px column to preserve. */
    {
        int mag = w / gwid;
        xw = mag < 2 ? 2 : mag > 8 ? 8 : mag;
        xw &= ~1;
    }

    xs = x0 < 0 ? 0 : x0;
    xe = x1 > SCR_W ? SCR_W : x1;
    if (xe > SCR_W - 8) xe = SCR_W - 8;             /* room for the widest span */

    { int skip = xs - x0;                            /* one mul+div, not per column */
      int n = skip * gwid;
      tx = n / w; hacc = n - tx * w; }

    /* Vertical clipping, the row base and the span are the SAME for every
     * column of this sprite - they were being recomputed per column, which
     * cost two multiplies and a divide each time. */
    {
        int ys0 = yt, ye0 = yb, err0 = 0, tskip = 0;
        uint8_t *rowbase;
        int span;

        if (ys0 < 0) {
            int n2 = (0 - yt) * ghei;
            int t0 = n2 / h;
            err0  = n2 - t0 * h;
            tskip = (t0 < ghei ? t0 : ghei - 1);
            ys0   = 0;
        }
        if (ye0 > VIEW_H) ye0 = VIEW_H;
        if (ys0 >= ye0) return;

        rowbase = fb + ys0 * SCR_W;          /* one multiply per sprite */
        span    = (ye0 - ys0) * SCR_W;       /* one multiply per sprite */

        for (int x = xs; x < xe; x += xw) {
            if (fwd < zbuf[x]) {
                const uint8_t *col, *tp, *tend;
                uint8_t *pp, *pend;
                int ix = tx;
                int err = err0;

                if (ix > gwid - 1) ix = gwid - 1;
                if (mirror) ix = gwid - 1 - ix;
                col  = g + ix * ghei;
                tend = col + ghei;
                tp   = col + tskip;

                pp   = rowbase + x;
                pend = pp + span;
                while (pp != pend) {
                    uint8_t t = *tp;
                    if (t) {
                        uint16_t cc = (uint16_t)rep3[lut[t]];  /* low half; no multiply */
                        uint16_t *q = (uint16_t *)pp;
                        q[0] = cc;
                        if (xw > 2) q[1] = cc;
                        if (xw > 4) { q[2] = cc; q[3] = cc; }
                    }
                    pp += SCR_W;
                    tp += vstep;
                    err += verr;
                    if (err >= h) { err -= h; tp++; }
                    if (tp >= tend) tp = tend - 1;
                }
            }
            for (int k = 0; k < xw; k++) {
                tx += hstep; hacc += herr;
                if (hacc >= w) { hacc -= w; tx++; }
            }
        }
    }
}

/* Gather actors, pickups and projectiles into one list, sort far to
 * near, then draw. They share a z-buffer with the walls. */
static void render_things(void)
{
    int n = 0;
    int cs = cosTab[pa], sn = SIN(pa);

    for (int i = 0; i < nact; i++) {
        int fwd;
        if (act[i].st == A_GONE) continue;
        fwd = ((act[i].x - px) * cs + (act[i].y - py) * sn) >> FIX;
        if (fwd < 96) continue;
        act[i].dist = fwd;
        rlist[n].dist = fwd; rlist[n].kind = K_ACT; rlist[n].idx = (uint8_t)i; n++;
    }
    for (int i = 0; i < npick; i++) {
        int fwd;
        if (!picks[i].on) continue;
        fwd = ((picks[i].x - px) * cs + (picks[i].y - py) * sn) >> FIX;
        if (fwd < 96) continue;
        picks[i].dist = fwd;
        rlist[n].dist = fwd; rlist[n].kind = K_PICK; rlist[n].idx = (uint8_t)i; n++;
    }
    for (int i = 0; i < MAXPROJ; i++) {
        int fwd;
        if (!projs[i].on) continue;
        fwd = ((projs[i].x - px) * cs + (projs[i].y - py) * sn) >> FIX;
        if (fwd < 64) continue;
        projs[i].dist = fwd;
        rlist[n].dist = fwd; rlist[n].kind = K_PROJ; rlist[n].idx = (uint8_t)i; n++;
    }

    for (int i = 1; i < n; i++) {                    /* far to near */
        rend_t v = rlist[i];
        int j = i - 1;
        while (j >= 0 && rlist[j].dist < v.dist) { rlist[j + 1] = rlist[j]; j--; }
        rlist[j + 1] = v;
    }

    for (int k = 0; k < n; k++) {
        int fwd = rlist[k].dist, i = rlist[k].idx;
        int dx, dy, rgt, lv;
        const uint8_t *lut;

        if (rlist[k].kind == K_ACT) {
            actor_t *a = &act[i];
            int frame, mirror = 0;
            dx = a->x - px; dy = a->y - py;
            rgt = (-dx * sn + dy * cs) >> FIX;
            lv = fwd >> 10; if (lv > 7) lv = 7;
            if (a->st == A_FIRE) lv = 0;
            lut = (a->type ? shadeE : shade)[lv];

            if (a->st == A_DIE || a->st == A_DEAD) {
                frame = F_DIE + a->frame;
            } else {
                int ang = point_angle(dx, dy);
                int d   = (ang - a->dir + NA / 2) & NA_MASK;
                int r8  = ((d + NA / 16) & NA_MASK) >> 8;
                int rot = rotMap[r8];
                mirror  = rotMirr[r8];
                frame = (a->st == A_FIRE) ? F_FIRE + rot
                                          : F_WALK + rot * 2 + (a->frame & 1);
            }
            draw_billboard(fwd, rgt, floorz(a->x >> FIX, a->y >> FIX), ONE,
                           eGfx[frame], SPR_W, SPR_H, lut, mirror);
        } else if (rlist[k].kind == K_PICK) {
            pick_t *q = &picks[i];
            dx = q->x - px; dy = q->y - py;
            rgt = (-dx * sn + dy * cs) >> FIX;
            lv = fwd >> 10; if (lv > 7) lv = 7;
            draw_billboard(fwd, rgt, floorz(q->x >> FIX, q->y >> FIX), ONE * 2 / 5,
                           pkGfx[q->type], PK_W, PK_H, shade[lv], 0);
        } else {
            proj_t *q = &projs[i];
            dx = q->x - px; dy = q->y - py;
            rgt = (-dx * sn + dy * cs) >> FIX;
            draw_billboard(fwd, rgt, q->z, ONE / 4,
                           pjGfx[q->frame], PJ_W, PJ_H, shade[0], 0);
        }
    }
}

/* ------------------------------------------------------------------
 * weapon, drawn at 2x into the bottom of the viewport
 * ------------------------------------------------------------------ */

static void render_weapon(void)
{
    const uint8_t *g = wGfx[wpnFrame];
    int ox = (SCR_W - WPN_W * 2) / 2 + bobX;
    int oy = VIEW_H - WPN_H * 2 + bobY;

    {
        const uint8_t *col = g;
        const uint8_t *top = wpnTop[wpnFrame], *bot = wpnBot[wpnFrame];
        for (int sx = 0; sx < WPN_W; sx++, col += WPN_H) {
            int dx = ox + sx * 2;
            int a  = top[sx], b = bot[sx];
            uint8_t *rp;
            if (a >= b) continue;                    /* empty column */
            if (dx < 0 || dx + 1 >= SCR_W) continue;
            if (oy < 0) continue;
            rp = back + (oy + a * 2) * SCR_W + dx;   /* one multiply per column */
            for (int sy = a; sy < b; sy++) {
                uint8_t c = col[sy];
                int yy = oy + sy * 2;
                if (c && (unsigned)yy < VIEW_H - 1) {
                    uint16_t cc = (uint16_t)c * 0x0101u;
                    *(uint16_t *)rp           = cc;
                    *(uint16_t *)(rp + SCR_W) = cc;
                }
                rp += SCR_W * 2;
            }
        }
    }
}

static void render_crosshair(void)
{
    uint8_t *fb = back;
    int cx = SCR_W / 2, cy = viewCy;
    uint8_t c = C(6, 30);

    {
        uint8_t *mid = fb + cy * SCR_W + cx;     /* one multiply total */
        for (int i = 3; i <= 7; i++) {
            mid[-i] = c;
            mid[ i] = c;
            if ((unsigned)(cy - i) < VIEW_H) mid[-i * SCR_W] = c;
            if ((unsigned)(cy + i) < VIEW_H) mid[ i * SCR_W] = c;
        }
    }
}

/* ------------------------------------------------------------------
 * HUD -- own 5x7 digits so the calculator and the desktop build agree
 * ------------------------------------------------------------------ */

static const uint8_t font5[10][5] = {
    { 0x3E, 0x51, 0x49, 0x45, 0x3E },   /* 0 */
    { 0x00, 0x42, 0x7F, 0x40, 0x00 },   /* 1 */
    { 0x42, 0x61, 0x51, 0x49, 0x46 },   /* 2 */
    { 0x21, 0x41, 0x45, 0x4B, 0x31 },   /* 3 */
    { 0x18, 0x14, 0x12, 0x7F, 0x10 },   /* 4 */
    { 0x27, 0x45, 0x45, 0x45, 0x39 },   /* 5 */
    { 0x3C, 0x4A, 0x49, 0x49, 0x30 },   /* 6 */
    { 0x01, 0x71, 0x09, 0x05, 0x03 },   /* 7 */
    { 0x36, 0x49, 0x49, 0x49, 0x36 },   /* 8 */
    { 0x06, 0x49, 0x49, 0x29, 0x1E },   /* 9 */
};

static uint8_t fontPix[10][35];
static uint8_t *fontPtr[10];      /* avoids the d*35 index multiply */   /* expanded at init: no shifts at draw time */

static void init_font(void)
{
    for (int d = 0; d < 10; d++) fontPtr[d] = fontPix[d];
    for (int d = 0; d < 10; d++)
        for (int cx = 0; cx < 5; cx++)
            for (int ry = 0; ry < 7; ry++)
                fontPix[d][cx * 7 + ry] = (font5[d][cx] >> ry) & 1;
}

/* Pointer walks only.  The obvious version tests bits with (1 << ry),
 * and indexes with yy * SCR_W - a variable shift, a mask and a multiply
 * per pixel, every one of which is a library CALL on the eZ80. */
static void draw_digit(int x, int y, int d, int sc, uint8_t c)
{
    const uint8_t *fp = fontPtr[d];
    uint8_t *col;
    int rowinc = sc * SCR_W;

    if (x < 0 || y < 0 || x + 5 * sc > SCR_W || y + 7 * sc > SCR_H) return;
    col = back + y * SCR_W + x;          /* the only multiply, once per digit */

    if (sc == 2) {                       /* the HUD's scale: no inner loops */
        uint16_t cc = (uint16_t)rep3[c];
        for (int cx = 0; cx < 5; cx++) {
            uint8_t *q = col;
            for (int ry = 0; ry < 7; ry++) {
                if (*fp++) {
                    *(uint16_t *)q           = cc;   /* 2x2 block, 2 stores */
                    *(uint16_t *)(q + SCR_W) = cc;
                }
                q += 2 * SCR_W;
            }
            col += 2;
        }
        return;
    }

    for (int cx = 0; cx < 5; cx++) {
        uint8_t *q = col;
        for (int ry = 0; ry < 7; ry++) {
            if (*fp++) {
                uint8_t *r = q;
                for (int b = 0; b < sc; b++) {
                    for (int a = 0; a < sc; a++) r[a] = c;
                    r += SCR_W;
                }
            }
            q += rowinc;
        }
        col += sc;
    }
}

static void draw_num(int x, int y, int v, int nd, int sc, uint8_t c)
{
    if (v < 0) v = 0;
    for (int i = nd - 1; i >= 0; i--) {
        draw_digit(x + i * (6 * sc), y, v % 10, sc, c);
        v /= 10;
    }
}

static void bar(int x, int y, int w, int h, int pct, uint8_t fg, uint8_t bg)
{
    int fill = (w * (pct > 100 ? 100 : pct < 0 ? 0 : pct)) / 100;
    uint8_t *row = back + y * SCR_W + x;         /* one multiply total */
    for (int j = 0; j < h; j++) {
        uint8_t *p = row;
        for (int i = 0; i < w; i++) *p++ = (i < fill) ? fg : bg;
        row += SCR_W;
    }
}

static void render_hud(void)
{
    int hy = VIEW_H;

    if (health != hudH || ammo != hudA || kills != hudK || curFps != hudF) {
        hudH = health; hudA = ammo; hudK = kills; hudF = curFps;
        hudDirty = 2;                      /* both buffers need it */
    }
    if (!hudDirty) return;
    hudDirty--;
    int by = SCR_H - 40;          /* widgets ride the bottom edge, as in v1 */

    fill_rect(0, hy, SCR_W, SCR_H - hy, C(2, 5));
    fill_rect(0, hy, SCR_W, 1, C(2, 11));
    fill_rect(0, by - 2, SCR_W, 1, C(2, 11));

    /* health: red cross, number, bar */
    fill_rect(12, by + 10, 2, 4, C(5, 28));
    fill_rect(11, by + 11, 4, 2, C(5, 28));
    draw_num(24, by + 8, health, 3, 2, C(5, 30));
    bar(24, by + 26, 76, 6, health, C(5, 26), C(2, 8));

    /* ammo: brass round, number, bar */
    fill_rect(124, by + 8, 1, 8, C(6, 24));
    fill_rect(125, by + 8, 1, 8, C(6, 28));
    fill_rect(126, by + 8, 1, 8, C(6, 20));
    draw_num(134, by + 8, ammo, 3, 2, C(6, 30));
    bar(134, by + 26, 76, 6, ammo * 2, C(6, 24), C(2, 8));

    /* kills, then fps, well clear of each other and the right edge */
    fill_rect(224, by + 12, 5, 2, C(0, 20));
    draw_num(232, by + 8, kills, 2, 2, C(0, 26));
    fill_rect(276, by + 12, 5, 2, C(3, 20));
    draw_num(284, by + 8, curFps > 99 ? 99 : curFps, 2, 2, C(3, 28));
}

/* ------------------------------------------------------------------
 * movement / input
 * ------------------------------------------------------------------ */

static bool solid_at(int fx, int fy)
{
    int mx = fx >> FIX, my = fy >> FIX;
    if ((unsigned)mx >= MAPW || (unsigned)my >= MAPH) return true;
    return worldRow[my][mx] != 0;
}

/* can the player stand here, coming from a floor at `from`? */
static bool can_enter(int fx, int fy, int from)
{
    int mx = fx >> FIX, my = fy >> FIX;
    int f, c;
    if ((unsigned)mx >= MAPW || (unsigned)my >= MAPH) return false;
    if (worldRow[my][mx]) return false;
    f = floorz(mx, my);
    c = ceilz(mx, my);
    if (f - from > MAXSTEP)  return false;     /* ledge too high  */
    if (c - f < HEADROOM)    return false;     /* shut door, crawlspace */
    return true;
}

static void move(int dx, int dy)
{
    const int r = 56;                    /* player radius, ~0.22 cell */
    int nx = px + dx, ny = py + dy;
    int fz = floorz(px >> FIX, py >> FIX);

    if (can_enter(nx + (dx > 0 ? r : -r), py, fz)) px = nx;
    fz = floorz(px >> FIX, py >> FIX);
    if (can_enter(px, ny + (dy > 0 ? r : -r), fz)) py = ny;
}

/* ---------- doors -------------------------------------------------- */

static bool cell_occupied(int mx, int my)
{
    if ((px >> FIX) == mx && (py >> FIX) == my) return true;
    for (int i = 0; i < nact; i++)
        if (act[i].st != A_DEAD && act[i].st != A_GONE &&
            (act[i].x >> FIX) == mx && (act[i].y >> FIX) == my) return true;
    return false;
}

static void update_doors(void)
{
    for (int i = 0; i < ndoor; i++) {
        door_t *d = &doors[i];
        if (d->hold) {
            if (--d->hold == 0) d->want = 0;
        }
        if (d->open < d->want) {
            d->open += 4;
            if (d->open > 64) d->open = 64;
        } else if (d->open > d->want) {
            if (cell_occupied(d->mx, d->my)) d->hold = 30;  /* stay open */
            else if (d->open >= 4) d->open -= 4;
            else d->open = 0;
        }
    }
}

/* open whatever door the player is facing */
static void use_door(void)
{
    int cs = cosTab[pa], sn = SIN(pa);
    for (int step = ONE / 2; step <= ONE * 3 / 2; step += ONE / 2) {
        int mx = (px + ((cs * step) >> FIX)) >> FIX;
        int my = (py + ((sn * step) >> FIX)) >> FIX;
        int d;
        if ((unsigned)mx >= MAPW || (unsigned)my >= MAPH) return;
        d = doorIdx[my][mx];
        if (d) {
            doors[d - 1].want = 64;
            doors[d - 1].hold = 150;
            return;
        }
    }
}

static void hurt_actor(actor_t *a, int dmg)
{
    if (a->st == A_DIE || a->st == A_DEAD) return;
    a->hp -= dmg;
    if (a->hp <= 0) {
        a->st = A_DIE; a->frame = 0; a->tics = 0;
        kills++;
    } else {
        a->st = A_PAIN; a->tics = 4;
    }
}

static void player_fire(void)
{
    int cs = cosTab[pa], sn = SIN(pa);
    actor_t *best = NULL;
    int bestd = 1 << 20;

    if (ammo <= 0 || wpnTic || deadTic) return;
    ammo--;
    wpnTic = 10;
    flashTic = 2;

    for (int i = 0; i < nact; i++) {
        actor_t *a = &act[i];
        int dx, dy, fwd, rgt;
        if (a->st == A_DIE || a->st == A_DEAD || a->st == A_GONE) continue;
        dx = a->x - px; dy = a->y - py;
        fwd = (dx * cs + dy * sn) >> FIX;
        if (fwd < 64) continue;
        rgt = (-dx * sn + dy * cs) >> FIX;
        if (rgt < 0) rgt = -rgt;
        if (rgt * 9 > fwd) continue;              /* ~6 degree cone     */
        if (fwd >= bestd) continue;
        if (!los(px, py, a->x, a->y)) continue;   /* no shooting through walls */
        bestd = fwd; best = a;
    }
    if (best) hurt_actor(best, 22 + (int)(rnd() & 15));
}

static void hurt_player(int dmg)
{
    if (deadTic) return;
    health -= dmg;
    painTic = 3;
    if (health <= 0) { health = 0; deadTic = 1; }
}

static void actor_move(actor_t *a, int dx, int dy)
{
    const int r = 48;
    int nx = a->x + dx, ny = a->y + dy;
    if (!solid_at(nx + (dx > 0 ? r : -r), a->y)) a->x = nx;
    if (!solid_at(a->x, ny + (dy > 0 ? r : -r))) a->y = ny;
}

static void spawn_proj(actor_t *a)
{
    int ang, cs, sn;
    for (int i = 0; i < MAXPROJ; i++) {
        proj_t *q = &projs[i];
        if (q->on) continue;
        ang = point_angle(px - a->x, py - a->y);
        cs = cosTab[ang]; sn = SIN(ang);
        q->x  = a->x + ((cs * 40) >> FIX);
        q->y  = a->y + ((sn * 40) >> FIX);
        q->z  = floorz(a->x >> FIX, a->y >> FIX) + ONE / 2;
        q->vx = (cs * PROJ_SPD) >> FIX;
        q->vy = (sn * PROJ_SPD) >> FIX;
        q->on = 1; q->life = 90; q->frame = 0;
        return;
    }
}

static void update_projectiles(void)
{
    for (int i = 0; i < MAXPROJ; i++) {
        proj_t *q = &projs[i];
        int dx, dy, mx, my;
        if (!q->on) continue;

        q->x += q->vx;
        q->y += q->vy;
        q->frame ^= 1;
        if (--q->life == 0) { q->on = 0; continue; }

        mx = q->x >> FIX; my = q->y >> FIX;
        if ((unsigned)mx >= MAPW || (unsigned)my >= MAPH) { q->on = 0; continue; }
        if (worldRow[my][mx]) { q->on = 0; continue; }
        if (q->z < floorz(mx, my) || q->z > ceilz(mx, my)) { q->on = 0; continue; }

        dx = q->x - px; dy = q->y - py;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx < 96 && dy < 96) {                 /* hit the player */
            q->on = 0;
            hurt_player(7 + (int)(rnd() & 7));
        }
    }
}

static void check_pickups(void)
{
    for (int i = 0; i < npick; i++) {
        pick_t *q = &picks[i];
        int dx, dy;
        if (!q->on) continue;
        dx = q->x - px; dy = q->y - py;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx > 110 || dy > 110) continue;

        if (q->type == P_HEALTH) {
            if (health >= 100) continue;          /* leave it for later */
            health += 25;
            if (health > 100) health = 100;
        } else {
            if (ammo >= 99) continue;
            ammo += 20;
            if (ammo > 99) ammo = 99;
        }
        q->on = 0;
        flashTic = 1;
    }
}

static void update_actors(void)
{
    for (int i = 0; i < nact; i++) {
        actor_t *a = &act[i];
        int dx, dy, adx, ady, adist;
        bool see;

        if (a->st == A_GONE) continue;
        if (a->st == A_DEAD) {                    /* linger, then fade */
            if (++a->tics >= CORPSE_LIFE) a->st = A_GONE;
            continue;
        }

        if (a->st == A_DIE) {                     /* death animation */
            if (++a->tics >= 5) {
                a->tics = 0;
                if (++a->frame >= 4) { a->frame = 4; a->st = A_DEAD; a->tics = 0; }
            }
            continue;
        }

        dx = px - a->x; dy = py - a->y;
        adx = dx < 0 ? -dx : dx; ady = dy < 0 ? -dy : dy;
        adist = (adx > ady) ? adx + (ady >> 1) : ady + (adx >> 1);
        see = !deadTic && adist < 16 * ONE && los(a->x, a->y, px, py);

        if (a->st == A_IDLE) {
            if (see) a->st = A_CHASE;
            continue;
        }
        if (a->st == A_PAIN) {
            if (--a->tics == 0) a->st = A_CHASE;
            continue;
        }
        if (a->st == A_FIRE) {
            if (--a->tics == 0) {
                if (see && adist < 13 * ONE) spawn_proj(a);
                a->st  = A_CHASE;
                a->atk = 26 + (uint8_t)(rnd() & 31);
            }
            continue;
        }

        /* chase: face the player, drift a little so you see other angles */
        if (((int)rnd() & 63) == 0) a->wob = (int8_t)(((int)rnd() % 3) - 1);
        a->dir = (point_angle(dx, dy) + a->wob * (NA / 16)) & NA_MASK;

        if (adist > 2 * ONE) {
            int sp = a->type ? 5 : 7;
            actor_move(a, (cosTab[a->dir] * sp) >> FIX,
                          (SIN(a->dir)    * sp) >> FIX);
        }
        if (++a->tics >= 6) { a->tics = 0; a->frame ^= 1; }

        if (a->atk) a->atk--;
        else if (see && adist < 11 * ONE && ((int)rnd() & 15) == 0) {
            a->st = A_FIRE; a->tics = 7;
        }
    }
}

static void set_palette(int which)
{
    const uint16_t *src;
    if (which == curPal) return;
    curPal = which;
    src = (which == 1) ? palPain : (which == 2) ? palFlash : palNormal;
    for (int i = 0; i < 256; i++) gfx_palette[i] = src[i];
}

/* ------------------------------------------------------------------ */

int main(void)
{
    bool running = true;

    init_world();

    gfx_Begin();
    gfx_SetDrawBuffer();
    back = BUF_B;

    init_palette();
    init_tables();
    init_textures();
    init_actor_gfx();
    init_font();

    timer_Control = TIMER1_ENABLE | TIMER1_32K | TIMER1_UP | TIMER1_NOINT;
    timer_1_Counter = 0;

    {
        unsigned last = 0;
    while (running) {
        const int SPD = moveSpeed, TURN = turnSpeed;
        unsigned tnow, dt;
        int cs, sn, moving = 0;

        kb_Scan();
        if (kb_Data[6] & kb_Clear) running = false;

        if (deadTic) {
            if (deadTic < 40) deadTic++;
            if (viewCy > 40) viewCy -= 3;         /* camera drops to the floor */
            /* init_world() must NOT be called again - re-entering it
             * faults (NMI, write to address 0). Death holds until quit. */
        } else {
            if (kb_Data[7] & kb_Left)  pa = (pa - TURN) & NA_MASK;
            if (kb_Data[7] & kb_Right) pa = (pa + TURN) & NA_MASK;

            cs = cosTab[pa]; sn = SIN(pa);

            if (kb_Data[7] & kb_Up) {
                move((cs * SPD) >> FIX, (sn * SPD) >> FIX); moving = 1;
            }
            if (kb_Data[7] & kb_Down) {
                move(-((cs * SPD) >> FIX), -((sn * SPD) >> FIX)); moving = 1;
            }
            if (kb_Data[1] & kb_Yequ) {
                move((sn * SPD) >> FIX, -((cs * SPD) >> FIX)); moving = 1;
            }
            if (kb_Data[1] & kb_Graph) {
                move(-((sn * SPD) >> FIX), (cs * SPD) >> FIX); moving = 1;
            }
                if (kb_Data[1] & kb_2nd)   player_fire();
            if (kb_Data[2] & kb_Alpha) use_door();
        }

        update_doors();
        update_actors();
        update_projectiles();
        if (!deadTic) check_pickups();

        /* eye height eases onto whatever floor we are standing on */
        {
            int target = floorz(px >> FIX, py >> FIX) + EYE_H;
            if (eyeZ < target) { eyeZ += 10; if (eyeZ > target) eyeZ = target; }
            else if (eyeZ > target) { eyeZ -= 10; if (eyeZ < target) eyeZ = target; }
        }

        /* weapon bob follows footsteps; settles when standing still */
        if (moving) {
            bobPhase = (bobPhase + 96) & NA_MASK;
            bobX = (cosTab[bobPhase] * 5) >> FIX;
            bobY = (SIN(bobPhase << 1) * 4) >> FIX;
            if (bobY < 0) bobY = -bobY;
        } else {
            if (bobX > 0) bobX--; else if (bobX < 0) bobX++;
            if (bobY > 0) bobY--;
        }

        if (wpnTic) {
            wpnTic--;
            wpnFrame = (wpnTic > 7) ? 1 : (wpnTic > 3) ? 2 : 0;
        } else {
            wpnFrame = 0;
        }

        set_palette(flashTic ? 2 : painTic ? 1 : 0);
        if (flashTic) flashTic--;
        if (painTic)  painTic--;

        render_walls();
        render_things();
        if (!deadTic) {
            render_crosshair();
            render_weapon();
        }
        render_hud();

        tnow = (unsigned)timer_1_Counter;
        dt   = tnow - last;
        last = tnow;
        if (dt) {
            static unsigned acc, n;         /* average over 8 frames so the */
            acc += dt; n++;                 /* readout does not dirty the   */
            if (n >= 8) { curFps = (32768u * 8) / acc; acc = 0; n = 0; }
        }

        gfx_SwapDraw();
        back = (back == BUF_A) ? BUF_B : BUF_A;
    }
    }

    set_palette(0);
    timer_Control = 0;
    gfx_End();
    return 0;
}
