/* ------------------------------------------------------------------
 * DOOMCE - Connect IQ port of the TI-84 Plus CE raycaster.
 *
 * Copyright (C) 2026 happyc0der.  GPL-3.0-or-later, see ../LICENSE.
 *
 * The CE engine paints the screen as vertical runs of one colour, 8 px
 * wide (fill_col8).  Connect IQ has no framebuffer, but it has
 * dc.fillRectangle - so every run becomes one fillRectangle call and the
 * algorithm carries over unchanged.  What changes is the cost model:
 * the CE was bound by bytes painted, this is bound by draw calls issued.
 * So the inner loops here walk *texels*, not pixel rows, and adjacent
 * runs of the same colour are merged before they reach the dc.
 *
 * All artwork is still generated procedurally at start-up; there are no
 * image resources.  Generation is chopped into chunks spread over the
 * first few frames so no single call can trip the Connect IQ watchdog.
 *
 * Fixed point and angles are exactly as in src/main.c: 1 cell == 256,
 * 2048 angle units per turn.  Monkey C Numbers are 32-bit, so none of
 * the eZ80's 24-bit contortions are needed.
 * ------------------------------------------------------------------ */
import Toybox.Graphics;
import Toybox.Lang;
import Toybox.Math;
import Toybox.System;
import Toybox.WatchUi;

/* ---------- configuration (Venu X1: 448 x 486 AMOLED) --------------- */

const SCR_W    = 448;
const SCR_H    = 486;
const VIEW_H   = 400;                 /* 3D viewport; the rest is HUD     */
const VIEW_CY  = 200;                 /* horizon                          */

const FIX      = 8;
const ONE      = 256;                 /* 1.0 == one map cell              */

const NA       = 2048;                /* angle units per full turn        */
const NA_MASK  = 2047;
const NA_90    = 512;

const PROJD    = 388;                 /* (SCR_W/2) / tan(30 deg)          */
const XSTEP    = 16;                  /* pixels per ray                   */
const XSHIFT   = 4;                   /* log2(XSTEP)                      */
const NRAY     = 28;                  /* SCR_W / XSTEP                    */
const DIST_MIN = 24;
const DIST_MAX = 16383;
const GUARD    = 64;                  /* max DDA steps per ray            */

const MAPW     = 24;
const MAPH     = 24;

const TEXW     = 64;
const TEXH     = 32;
const NWALLTEX = 4;

const EYE_H    = 160;                 /* ONE * 5 / 8                      */
const MAXSTEP  = 96;                  /* ONE * 3 / 8                      */
const HEADROOM = 128;                 /* ONE / 2                          */
const MAXDOOR  = 8;

const SPR_W    = 28;
const SPR_H    = 36;
const SPR_SZ   = 1008;
const NROT     = 5;
const F_WALK   = 0;
const F_FIRE   = 10;
const F_DIE    = 15;
const NFRAME   = 20;
const MAXACT   = 20;

const A_IDLE = 0; const A_CHASE = 1; const A_FIRE = 2; const A_PAIN = 3;
const A_DIE  = 4; const A_DEAD  = 5; const A_GONE = 6;
const CORPSE_LIFE = 400;

const MAXPICK  = 14;
const PK_W     = 20;
const PK_H     = 20;
const P_HEALTH = 0;
const P_AMMO   = 1;

const MAXPROJ  = 12;
const PJ_W     = 12;
const PJ_H     = 12;
const PROJ_SPD = 10;                  /* CE: 30 at ~6 fps; this runs ~20   */

const WPN_W    = 48;
const WPN_H    = 36;
const NWPN     = 3;
const WPN_S    = 4;                   /* weapon drawn at 4x               */

const K_ACT = 0; const K_PICK = 1; const K_PROJ = 2;
const KEY_WALL = 255;                 /* cellKey of a solid cell           */
const KEY_DOOR = 200;                 /* cellKey of door i is KEY_DOOR + i */

/* input actions */
const ACT_NONE = 0; const ACT_FWD = 1; const ACT_BACK = 2; const ACT_LEFT = 3;
const ACT_RIGHT = 4; const ACT_SLEFT = 5; const ACT_SRIGHT = 6; const ACT_USE = 7;
const ACT_FIRE = 8;
const BURST    = 8;                   /* frames of movement per tap       */
const PROFILE  = true;                /* print per-frame cost to the console */
const BENCH    = false;               /* micro-benchmark the dc on first frames */

/* colour bytes used in the hot path: C(ramp, s) == (ramp << 5) | s */
const COL_FLOOR = 85;                 /* C(2, 21) */
const COL_CEIL  = 141;                /* C(4, 13) */
const COL_XHAIR = 222;                /* C(6, 30) */

/* start-up chunks */

class Engine {
    /* ---------- tables ---------- */
    var cosTab;  var icosTab;         /* [NA]                              */
    var colAng;  var colCos;          /* [NRAY]                            */
    var atanTab;                      /* [65]                              */
    var shade;   var shadeE;          /* [8] of ByteArray[256]             */
    var pal;                          /* [3] of Array[256] rgb             */
    var shadeRGB; var shadeERGB;      /* [3][8] of Array[256] rgb          */
    var wallTex;                      /* ByteArray [tex][x][y]             */
    var texBmp;                       /* [NWALLTEX*8] BufferedBitmap per (tex, shade) */
    var sprDefs;                      /* [[gfx, base, w, h, mirror], ...]  */
    var sprBmp; var sprIdx; var sprPal; var sprKey;   /* per sprite bitmap */
    var sprMap;                       /* [NFRAME*2] frame*2+mirror -> bitmap index */
    var PK0 = 0; var PJ0 = 0;         /* first pickup / projectile bitmap  */
    var cellKey;                      /* ByteArray: equal keys => same floor/ceiling */
    var tintPain = null; var tintFlash = null;
    var projd = PROJD;
    var projScale = PROJD * ONE;
    var moveSpeed = 7;                /* 8.8 cells per frame               */
    var turnSpeed = 13;               /* angle units per frame             */

    /* ---------- world ---------- */
    var world; var hmap; var doorIdx; /* ByteArray [MAPH*MAPW]             */
    var doorMx; var doorMy; var doorOpen; var doorWant; var doorHold;
    var ndoor = 0;
    var eyeZ = 0;

    /* ---------- actors / pickups / projectiles (parallel arrays) ---------- */
    var aX; var aY; var aDir; var aDist; var aHp; var aType; var aSt;
    var aFrame; var aTics; var aAtk; var aWob;
    var nact = 0;
    var eGfx;                         /* ByteArray [NFRAME][SPR_W][SPR_H]  */

    var pX; var pY; var pType; var pOn; var pDist;
    var npick = 0;
    var pkGfx;                        /* ByteArray [2][PK_W][PK_H]         */

    var jX; var jY; var jVx; var jVy; var jZ; var jOn; var jLife; var jFrame; var jDist;
    var pjGfx;                        /* ByteArray [2][PJ_W][PJ_H]         */

    var rDist; var rKind; var rIdx;   /* draw list                         */

    /* ---------- weapon ---------- */
    var wGfx;                         /* ByteArray [NWPN][WPN_W][WPN_H]    */
    var wpnTop; var wpnBot;           /* [NWPN] of ByteArray[WPN_W]        */
    var wpnBmp = null;                /* [3 pal][NWPN] BufferedBitmap refs */
    var wpnTic = 0; var wpnFrame = 0; var bobPhase = 0; var bobX = 0; var bobY = 0;
    var flashTic = 0; var painTic = 0;

    /* ---------- player ---------- */
    var px = 0; var py = 0; var pa = 0;
    var health = 100; var ammo = 50; var kills = 0;
    var viewCy = VIEW_CY;
    var deadTic = 0;
    var curPal = 0;

    /* ---------- input ---------- */
    var burst;                        /* [9] frames left per action        */
    var held = ACT_NONE;
    var wantFire = false; var wantUse = false; var wantRestart = false;

    /* ---------- per-frame scratch ---------- */
    var zbuf;                         /* [NRAY]                            */
    var mLastCol = -1;
    var mCalls = 0;                   /* fillRectangle calls this frame    */
    var curFps = 0; var fpsAcc = 0; var fpsN = 0; var lastT = 0;
    var frameCalls = 0;
    var mIters = 0;                   /* hot-loop iterations this frame (profiling) */

    /* ---------- procedural art scratch ---------- */
    var gspr; var gbase = 0; var gw = 0; var gh = 0; var gexp = 0; var gforce = 0;
    var rng = 0x1234AB;

    var initStep = 0;
    var logo = null;                  /* title logo, loaded on first draw   */
    var benchStep = 0; var benchBmp = null;
    var tWalls = 0; var tThings = 0; var tWeapon = 0; var tHud = 0;

    /* 8 colour ramps x 32 brightness steps = 256 palette entries. */
    var rampRGB = [ [200,200,200], [195,85,65], [150,105,55], [116,124,92],
                    [110,126,150], [225,50,50], [235,205,85], [225,170,140] ];

    var mapSrc = [
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
        "111111111111111111111111" ];

    /* '.' default; '1'..'5' raise the floor in 1/8 steps; 'a'..'e' lower
     * the ceiling; 'D' is a door. */
    var hSrc = [
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
        "........................" ];

    var rotMap  = [0, 1, 2, 3, 4, 3, 2, 1];
    var rotMirr = [0, 0, 0, 0, 0, 1, 1, 1];

    function initialize() {
        cosTab  = new [NA];
        icosTab = new [NA];
        colAng  = new [NRAY];
        colCos  = new [NRAY];
        atanTab = new [65];
        zbuf    = new [NRAY];
        wallTex = new [NWALLTEX * TEXW * TEXH]b;
        eGfx    = new [NFRAME * SPR_SZ]b;
        pkGfx   = new [2 * PK_W * PK_H]b;
        pjGfx   = new [2 * PJ_W * PJ_H]b;
        wGfx    = new [NWPN * WPN_W * WPN_H]b;
        wpnTop  = [ new [WPN_W]b, new [WPN_W]b, new [WPN_W]b ];
        wpnBot  = [ new [WPN_W]b, new [WPN_W]b, new [WPN_W]b ];
        world   = new [MAPW * MAPH]b;
        hmap    = new [MAPW * MAPH]b;
        doorIdx = new [MAPW * MAPH]b;
        doorMx = new [MAXDOOR]; doorMy = new [MAXDOOR]; doorOpen = new [MAXDOOR];
        doorWant = new [MAXDOOR]; doorHold = new [MAXDOOR];
        aX = new [MAXACT]; aY = new [MAXACT]; aDir = new [MAXACT]; aDist = new [MAXACT];
        aHp = new [MAXACT]; aType = new [MAXACT]; aSt = new [MAXACT]; aFrame = new [MAXACT];
        aTics = new [MAXACT]; aAtk = new [MAXACT]; aWob = new [MAXACT];
        pX = new [MAXPICK]; pY = new [MAXPICK]; pType = new [MAXPICK]; pOn = new [MAXPICK]; pDist = new [MAXPICK];
        jX = new [MAXPROJ]; jY = new [MAXPROJ]; jVx = new [MAXPROJ]; jVy = new [MAXPROJ]; jZ = new [MAXPROJ];
        jOn = new [MAXPROJ]; jLife = new [MAXPROJ]; jFrame = new [MAXPROJ]; jDist = new [MAXPROJ];
        var nr = MAXACT + MAXPICK + MAXPROJ;
        rDist = new [nr]; rKind = new [nr]; rIdx = new [nr];
        burst = new [9];
        cellKey = new [MAPW * MAPH]b;
        texBmp  = new [NWALLTEX * 8];
        /* sprite bitmaps: 20 soldier frames, mirrors for rotations 1..3,
         * 2 pickups, 2 projectiles */
        sprDefs = [];
        sprMap  = new [NFRAME * 2];
        for (var f = 0; f < NFRAME; f++) {
            sprMap[f * 2] = sprDefs.size(); sprMap[f * 2 + 1] = sprDefs.size();
            sprDefs.add([eGfx, f * SPR_SZ, SPR_W, SPR_H, 0]);
        }
        for (var r = 1; r <= 3; r++) {
            for (var k = 0; k < 2; k++) {
                var f = F_WALK + r * 2 + k;
                sprMap[f * 2 + 1] = sprDefs.size();
                sprDefs.add([eGfx, f * SPR_SZ, SPR_W, SPR_H, 1]);
            }
            var ff = F_FIRE + r;
            sprMap[ff * 2 + 1] = sprDefs.size();
            sprDefs.add([eGfx, ff * SPR_SZ, SPR_W, SPR_H, 1]);
        }
        PK0 = sprDefs.size();
        sprDefs.add([pkGfx, 0, PK_W, PK_H, 0]);
        sprDefs.add([pkGfx, PK_W * PK_H, PK_W, PK_H, 0]);
        PJ0 = sprDefs.size();
        sprDefs.add([pjGfx, 0, PJ_W, PJ_H, 0]);
        sprDefs.add([pjGfx, PJ_W * PJ_H, PJ_W, PJ_H, 0]);
        var ns = sprDefs.size();
        sprBmp = new [ns]; sprIdx = new [ns]; sprPal = new [ns]; sprKey = new [ns];
        if (Graphics has :createColor) {
            tintPain  = Graphics.createColor(110, 255, 40, 20);
            tintFlash = Graphics.createColor(70, 255, 240, 200);
        }
        for (var i = 0; i < 9; i++) { burst[i] = 0; }
        for (var i = 0; i < MAXPROJ; i++) { jOn[i] = 0; }
        for (var i = 0; i < MAXACT; i++) { aSt[i] = A_GONE; }
        for (var i = 0; i < MAXPICK; i++) { pOn[i] = 0; }
        ndoor = 0;
    }

    /* ------------------------------------------------------------------
     * start-up, one chunk per frame
     * ------------------------------------------------------------------ */

    /* Start-up work list.  The Connect IQ watchdog kills any single
     * callback that runs more than roughly 10k loop iterations, so
     * generation is cut into ~50 tasks and one runs per frame. */
    var tasks = null;
    var taskN = 0;

    function buildTasks() {
        var t = [];
        t.add([:initPalette0, 0]);
        for (var l = 0; l < 8; l++) { t.add([:initShade, l]); }
        for (var i = 0; i < 4; i++) { t.add([:initCos, i]); }
        t.add([:initTables2, 0]);
        for (var i = 0; i < 8; i++) { t.add([:initTexCols, i]); }
        for (var f = 0; f < NFRAME; f++) { t.add([:initSprite, f * 2]); t.add([:initSprite, f * 2 + 1]); }
        for (var f = 0; f < NWPN; f++) { t.add([:initWeapon, f * 2]); t.add([:initWeapon, f * 2 + 1]); }
        t.add([:initPickupGfx, 0]);
        for (var i = 0; i < NWALLTEX * 8; i++) { t.add([:initTexBitmap, i]); }
        for (var i = 0; i < sprDefs.size(); i++) { t.add([:initSpriteBitmap, i]); }
        for (var f = 0; f < NWPN; f++) { t.add([:initWeaponBitmap, f]); }
        t.add([:initWorld, 0]);
        tasks = t; taskN = t.size();
    }

    /* a few tasks per tick, while the tick stays short */
    function initChunk() {
        if (tasks == null) { buildTasks(); }
        var t0 = System.getTimer(); var n = 0;
        while (initStep < taskN && n < 4 && System.getTimer() - t0 < 30) {
            var t = tasks[initStep];
            method(t[0]).invoke(t[1]);
            initStep++; n++;
        }
    }

    function initDone() { return tasks != null && initStep >= taskN; }

    function rgb(r, g, b) { return (r << 16) | (g << 8) | b; }

    function initPalette0(unused) {
        pal = [ new [256], new [256], new [256] ];
        var pn = pal[0]; var pp = pal[1]; var pf = pal[2];
        for (var r = 0; r < 8; r++) {
            var rr = rampRGB[r];
            for (var s = 0; s < 32; s++) {
                var i = (r << 5) | s;
                var R = rr[0] * s / 31; var G = rr[1] * s / 31; var B = rr[2] * s / 31;
                pn[i] = rgb(R, G, B);
                /* damage: push everything toward red.  flash: lift everything. */
                var pr = R + (255 - R) * 3 / 5;
                pp[i] = rgb(pr, G / 3, B / 3);
                pf[i] = rgb(R + (255 - R) / 3, G + (255 - G) / 3, B + (255 - B) / 4);
            }
        }
        shade  = new [8];
        shadeE = new [8];
        shadeRGB  = new [8];
        shadeERGB = new [8];
    }

    /* one shade level: index remap plus the three palette-resolved tables */
    /* one shade level: index remap plus the palette-resolved RGB tables */
    function initShade(l) {
        var sh = new [256]b; var she = new [256]b;
        var mul = 16 - l * 2;
        var src = pal[0];
        var a = new [256]; var b = new [256];
        for (var c = 0; c < 256; c++) {
            var r = c >> 5;
            /* scale, don't subtract: keeps relative contrast at range */
            var v = ((c & 31) * mul) >> 4;
            if (v > 31) { v = 31; }
            sh[c]  = (r << 5) | v;
            she[c] = ((r == 3 ? 0 : r) << 5) | v;
            a[c] = src[sh[c]]; b[c] = src[she[c]];
        }
        shade[l] = sh; shadeE[l] = she;
        shadeRGB[l] = a; shadeERGB[l] = b;
    }

    function initCos(q) {
        var k = 2.0 * Math.PI / NA;
        var i0 = q * (NA / 4); var i1 = i0 + NA / 4;
        for (var i = i0; i < i1; i++) {
            var c  = Math.cos(i * k);
            var ac = c < 0 ? -c : c;
            cosTab[i] = (c * 256.0 + (c < 0 ? -0.5 : 0.5)).toNumber();
            var v = (ac < 0.00002) ? DIST_MAX + 1 : (256.0 / ac).toNumber();
            icosTab[i] = v > DIST_MAX + 1 ? DIST_MAX + 1 : v;
        }
    }

    function initTables2(unused) {
        var k = 2.0 * Math.PI / NA;
        projScale = projd * ONE;
        for (var r = 0; r < NRAY; r++) {
            var x = r << XSHIFT;
            var t = Math.atan((x - SCR_W / 2 + 0.5) / projd.toFloat());
            var u = t / k;
            colAng[r] = (u + (u < 0 ? -0.5 : 0.5)).toNumber();
            colCos[r] = (Math.cos(t) * 256.0 + 0.5).toNumber();
        }
        for (var i = 0; i <= 64; i++) {
            atanTab[i] = (Math.atan(i / 64.0) / k + 0.5).toNumber();
        }
    }

    function rnd() {
        var s = rng;
        s = (s ^ (s << 5)) & 0xFFFFFF;        /* 24-bit state, as on the eZ80 */
        s = s ^ (s >> 7);
        s = (s ^ (s << 3)) & 0xFFFFFF;
        rng = s;
        return s;
    }

    function initTexCols(q) {
        var tex = wallTex;
        var t0 = 0; var t1 = TEXW * TEXH; var t2 = 2 * TEXW * TEXH; var t3 = 3 * TEXW * TEXH;
        for (var x = q * 8; x < q * 8 + 8; x++) {
            var xo = x * TEXH;
            for (var y = 0; y < TEXH; y++) {
                var n = rnd() & 3;
                var i = xo + y;
                /* brick */
                var row = y >> 4;
                var bx  = (x + (((row & 1) != 0) ? 16 : 0)) & 31;
                tex[t0 + i] = ((y & 15) < 2 || bx < 2) ? (11 + n) : ((1 << 5) | (19 + n));
                /* rough stone */
                tex[t1 + i] = 16 + n + ((((x ^ y) & 8) != 0) ? 3 : 0);
                /* wood planks with vertical grain */
                tex[t2 + i] = ((x & 15) == 0) ? ((2 << 5) | 7) : ((2 << 5) | (18 + n + (((x * 5) & 7) >> 1)));
                /* tech panel */
                var e    = (x & 31) < 2 || (y & 31) < 2;
                var lamp = (x & 31) > 12 && (x & 31) < 20 && (y & 31) > 12 && (y & 31) < 20;
                tex[t3 + i] = lamp ? ((6 << 5) | (26 + n)) : (e ? 8 : (15 + n));
            }
        }
    }

    /* ------------------------------------------------------------------
     * sprite painting helpers  (frames are column-major, like the walls)
     * ------------------------------------------------------------------ */

    function gpx(x, y, c) {
        if (x >= 0 && x < gw && y >= 0 && y < gh) { gspr[gbase + x * gh + y] = c; }
    }

    function grect(x0, y0, x1, y1, c) {
        if (gforce != 0) { c = gforce; }
        var e = gexp;
        for (var x = x0 - e; x <= x1 + e; x++) {
            for (var y = y0 - e; y <= y1 + e; y++) { gpx(x, y, c); }
        }
    }

    /* ellipse lit from the upper left, so limbs and torsos read as round */
    function gell(cx, cy, rx, ry, ramp, base) {
        rx += gexp; ry += gexp;
        if (rx < 1) { rx = 1; }
        if (ry < 1) { ry = 1; }
        var force = gforce;
        for (var x = cx - rx; x <= cx + rx; x++) {
            var ux = ((x - cx) << 8) / rx;
            for (var y = cy - ry; y <= cy + ry; y++) {
                var uy = ((y - cy) << 8) / ry;
                if (ux * ux + uy * uy > 65536) { continue; }
                if (force != 0) { gpx(x, y, force); continue; }
                var l = base + ((-ux - uy) >> 6);
                if (l < 2)  { l = 2; }
                if (l > 31) { l = 31; }
                gpx(x, y, (ramp << 5) | l);
            }
        }
    }

    function gclear(n) {
        for (var i = 0; i < n; i++) { gspr[gbase + i] = 0; }
    }

    /* enemy artwork: generic WW2-era infantry - field-grey tunic,
     * coal-scuttle helmet, bolt-action rifle.  Painted twice: expanded in
     * near-black, then normally, which gives a 1px outline for free. */
    function soldierBody(rot, legph, firing) {
        var uni = 3; var ub = 21;
        var trx  = [6, 6, 4, 5, 6];
        var lean = [0, 1, 2, 1, 0];
        var cx   = 14 + lean[rot];
        var rx   = trx[rot];
        var back = (rot >= 3);
        var side = (rot == 2);
        var cu = (uni << 5);

        for (var i = 0; i < 2; i++) {                                  /* legs */
            var lx  = cx + ((i != 0) ? (side ? 2 : 4) : (side ? -2 : -4));
            var ph  = (legph ^ i) & 1;
            var bot = 31 - ((ph != 0) ? 2 : 0);
            grect(lx - 2, 23, lx + 1, bot, cu | (ub - 6));
            grect(lx - 2, bot + 1, lx + 1, 35, (2 << 5) | 7);
        }
        gell(cx, 18, rx, 7, uni, ub);                                  /* torso */
        if (back) {                                  /* rucksack + rolled blanket */
            gell(cx, 17, rx - 1, 5, 2, 11);
            grect(cx - rx + 1, 13, cx + rx - 1, 14, (2 << 5) | 14);
        }
        if (side) {                                                    /* arms */
            grect(cx + 2, 14, cx + 4, 23, cu | (ub - 7));
        } else {
            grect(cx - rx - 2, 14, cx - rx, 23, cu | (ub - 7));
            grect(cx + rx,     14, cx + rx + 2, 23, cu | (ub - 7));
        }
        grect(cx - rx, 22, cx + rx, 23, (2 << 5) | 5);                 /* belt */

        if (firing != 0) {                                             /* rifle */
            if (back) {
                grect(cx - 3, 15, cx + 3, 17, cu | (ub - 4));
                grect(cx - 1, 13, cx + 1, 15, (4 << 5) | 15);
            } else {
                grect(cx - 4, 15, cx + 7, 17, cu | (ub - 4));
                grect(cx + 1, 13, 27, 14, (2 << 5) | 9);
                grect(cx + 6, 12, 27, 12, (4 << 5) | 17);
            }
        } else if (side) {
            grect(cx + 1, 13, 26, 14, (2 << 5) | 9);
            grect(cx + 5, 12, 26, 12, (4 << 5) | 15);
        } else if (!back) {
            grect(cx + rx + 1, 9, cx + rx + 2, 26, (2 << 5) | 8);
            grect(cx + rx,    11, cx + rx + 3, 12, (4 << 5) | 14);
        }

        if (back) {                                                    /* head */
            gell(cx, 12, 4, 3, uni, ub - 8);
        } else if (side) {
            gell(cx + 1, 12, 3, 3, 7, 24);
            gpx(cx + 4, 12, (7 << 5) | 27);
            gpx(cx + 2, 12, 3);
        } else {
            gell(cx, 12, 4, 3, 7, 24);
            if (rot == 0) { gpx(cx - 2, 12, 3); gpx(cx + 2, 12, 3); }
            else          { gpx(cx - 1, 12, 3); gpx(cx + 3, 12, 3); }
        }
        gell(cx, 7, side ? 5 : 6, 4, 0, back ? 16 : 19);               /* helmet */
        grect(cx - (side ? 5 : 7), 9, cx + (side ? 5 : 7), 10, back ? 12 : 15);
    }

    /* task = frame*2 + pass.  pass 0: clear + outline, pass 1: body */
    function initSprite(task) {
        var frame = task >> 1; var pass = task & 1;
        gspr = eGfx; gbase = frame * SPR_SZ; gw = SPR_W; gh = SPR_H;
        if (pass == 0) { gclear(SPR_SZ); gexp = 1; gforce = 2; }
        else           { gexp = 0; gforce = 0; }
        if (frame < F_FIRE)      { soldierBody((frame - F_WALK) / 2, (frame - F_WALK) % 2, 0); }
        else if (frame < F_DIE)  { soldierBody(frame - F_FIRE, 0, 1); }
        else                     { deathBody(frame - F_DIE); }
    }

    /* death: the body folds forward and goes down, helmet comes off at the end */
    function deathBody(f) {
        var uni = 3; var ub = 21; var cu = (uni << 5);
        if (f >= 3) { gell(15, 34, 10 + f, 2, 5, 12); }                /* pool */
        if (f == 0) {                                                  /* staggered */
            grect(10, 24, 13, 35, cu | (ub - 6));
            grect(15, 24, 18, 35, cu | (ub - 6));
            gell(14, 19, 6, 7, uni, ub);
            gell(15, 12, 4, 3, 7, 22);
            gell(15,  8, 6, 4, 0, 18);
            grect(8, 10, 22, 11, 14);
        } else if (f == 1) {                                           /* doubled over */
            grect(11, 26, 14, 35, cu | (ub - 6));
            grect(16, 26, 19, 35, cu | (ub - 6));
            gell(15, 22, 7, 6, uni, ub - 1);
            gell(19, 17, 4, 3, 7, 21);
            gell(20, 14, 6, 4, 0, 17);
        } else if (f == 2) {                                           /* on knees */
            grect(9, 29, 20, 35, cu | (ub - 7));
            gell(15, 26, 8, 5, uni, ub - 2);
            gell(21, 23, 4, 3, 7, 20);
            gell(22, 21, 5, 4, 0, 16);
        } else if (f == 3) {                                           /* prone */
            gell(15, 31, 11, 4, uni, ub - 3);
            grect(5, 30, 10, 34, cu | (ub - 6));
            gell(24, 30, 4, 3, 7, 19);
            gell(7, 28, 4, 3, 0, 15);
        } else {                                                       /* flat */
            gell(15, 33, 12, 3, uni, ub - 5);
            grect(4, 32, 9, 35, cu | (ub - 8));
            gell(25, 33, 4, 2, 7, 18);
            gell(6, 30, 4, 2, 0, 14);
        }
    }

    /* the player's rifle, three frames */
    function gunBody(f) {
        var lift = (f == 1) ? 4 : ((f == 2) ? 2 : 0);                   /* recoil */
        grect(10, 27 - lift, 33, 35,        (2 << 5) | 15);            /* stock    */
        grect(14, 23 - lift, 31, 28 - lift, (2 << 5) | 18);
        grect(18, 15 - lift, 30, 25 - lift, 17);                       /* receiver */
        grect(31, 19 - lift, 37, 22 - lift, 16);                       /* bolt     */
        grect(22, 24 - lift, 28, 30 - lift, 10);                       /* magazine */
        grect(21,  2 - lift, 27, 17 - lift, (4 << 5) | 22);            /* barrel   */
        grect(20,  1 - lift, 28,  4 - lift, 15);                       /* sight    */
        grect(23,  0 - lift, 25,  3 - lift, 20);                       /* blade    */
        gell(19, 26 - lift, 4, 3, 7, 25);                              /* fore hand    */
        gell(28, 31 - lift, 4, 3, 7, 23);                              /* trigger hand */
    }

    function initWeapon(task) {
        var f = task >> 1; var pass = task & 1;
        gspr = wGfx; gw = WPN_W; gh = WPN_H;
        gbase = f * WPN_W * WPN_H;
        if (pass == 0) {
            gclear(WPN_W * WPN_H);
            gexp = 1; gforce = 2;  gunBody(f);
            return;
        }
        gexp = 0; gforce = 0;  gunBody(f);
        if (f == 1) {                                                  /* muzzle flash */
            gell(24, 3, 9, 6, 6, 31);
            gell(24, 2, 5, 4, 6, 31);
            grect(22, 0, 26, 9, (6 << 5) | 31);
            grect(15, 2, 33, 3, (6 << 5) | 27);
            grect(18, 0, 30, 1, (6 << 5) | 24);
        }
        /* first/last non-transparent row per column */
        var top = wpnTop[f]; var bot = wpnBot[f];
        for (var cx = 0; cx < WPN_W; cx++) {
            var cb = gbase + cx * WPN_H;
            var a = 0; var b = WPN_H;
            while (a < WPN_H && wGfx[cb + a] == 0) { a++; }
            while (b > a && wGfx[cb + b - 1] == 0) { b--; }
            top[cx] = a; bot[cx] = b;
        }
    }

    function initPickupGfx(unused) {
        gspr = pkGfx; gw = PK_W; gh = PK_H;
        gbase = P_HEALTH * PK_W * PK_H;                                /* medical case */
        gclear(PK_W * PK_H);
        gexp = 1; gforce = 2; grect(2, 5, 17, 18, 0); gexp = 0; gforce = 0;
        grect(2, 5, 17, 18, 26);
        grect(2, 5, 17, 7,  20);
        grect(8, 9, 11, 16, (5 << 5) | 29);
        grect(5, 11, 14, 14, (5 << 5) | 29);
        grect(8, 3, 11, 5,  14);

        gbase = P_AMMO * PK_W * PK_H;                                  /* ammo crate */
        gclear(PK_W * PK_H);
        gexp = 1; gforce = 2; grect(2, 6, 17, 18, 0); gexp = 0; gforce = 0;
        grect(2, 6, 17, 18, (2 << 5) | 14);
        grect(2, 6, 17, 8,  (2 << 5) | 18);
        for (var i = 0; i < 4; i++) { grect(4 + i * 4, 10, 6 + i * 4, 16, (6 << 5) | 22); }

        gspr = pjGfx; gw = PJ_W; gh = PJ_H;                            /* enemy bolt */
        for (var f = 0; f < 2; f++) {
            var r = (f != 0) ? 5 : 4;
            gbase = f * PJ_W * PJ_H;
            gclear(PJ_W * PJ_H);
            gell(6, 6, r, r, 5, 22);
            gell(6, 6, r - 2, r - 2, 6, 31);
        }
    }

    /* The weapon is the one sprite that never scales or clips, so it is
     * pre-rendered once per (palette, frame) into a small palette bitmap
     * and blitted with a single drawScaledBitmap per frame.  If the device
     * cannot make buffered bitmaps, renderWeapon falls back to runs. */
    /* one 64x32 palette bitmap per (texture, shade level).  Drawn as runs
     * down each column; the palette is that texture's colours at that
     * shade, so drawing needs no lookup at all. */
    function initTexBitmap(task) {
        var tex = task >> 3; var lv = task & 7;
        var lut = shadeRGB[lv];
        var base = tex * TEXW * TEXH;
        var seen = new [256]b;
        for (var i = 0; i < 256; i++) { seen[i] = 255; }
        var plist = [];
        for (var i = 0; i < TEXW * TEXH; i++) {
            var c = wallTex[base + i];
            if (seen[c] == 255) { seen[c] = plist.size(); plist.add(lut[c]); }
        }
        var ref = Graphics.createBufferedBitmap({ :width => TEXW, :height => TEXH, :palette => plist });
        var bmp = ref.get();
        var bdc = bmp.getDc();
        var last = -1;
        for (var cx = 0; cx < TEXW; cx++) {
            var cb = base + cx * TEXH;
            var y = 0;
            while (y < TEXH) {
                var c = wallTex[cb + y];
                var y1 = y + 1;
                while (y1 < TEXH && wallTex[cb + y1] == c) { y1++; }
                if (c != last) { bdc.setColor(lut[c], Graphics.COLOR_TRANSPARENT); last = c; }
                bdc.fillRectangle(cx, y, 1, y1 - y);
                y = y1;
            }
        }
        texBmp[task] = bmp;
    }

    /* one palette bitmap per sprite frame.  Entry 0 is transparent; the
     * rest are the frame's colour bytes, so shading a sprite is a
     * setPalette with those bytes run through the shade table. */
    function initSpriteBitmap(k) {
        var d = sprDefs[k];
        var g = d[0]; var base = d[1]; var w = d[2]; var h = d[3]; var mirror = d[4];
        var seen = new [256]b;
        for (var i = 0; i < 256; i++) { seen[i] = 255; }
        seen[0] = 0;
        var idx = [0];
        for (var i = 0; i < w * h; i++) {
            var c = g[base + i];
            if (seen[c] == 255) { seen[c] = idx.size(); idx.add(c); }
        }
        var n = idx.size();
        var plist = new [n];
        var lut = shadeRGB[0];
        plist[0] = Graphics.COLOR_TRANSPARENT;
        for (var i = 1; i < n; i++) { plist[i] = lut[idx[i]]; }
        var ref = Graphics.createBufferedBitmap({ :width => w, :height => h, :palette => plist });
        var bmp = ref.get();
        var bdc = bmp.getDc();
        var last = -1;
        for (var cx = 0; cx < w; cx++) {
            var sx = (mirror != 0) ? (w - 1 - cx) : cx;
            var cb = base + sx * h;
            var y = 0;
            while (y < h) {
                var c = g[cb + y];
                var y1 = y + 1;
                while (y1 < h && g[cb + y1] == c) { y1++; }
                if (c != 0) {
                    if (c != last) { bdc.setColor(lut[c], Graphics.COLOR_TRANSPARENT); last = c; }
                    bdc.fillRectangle(cx, y, 1, y1 - y);
                }
                y = y1;
            }
        }
        var ib = new [n]b;
        for (var i = 0; i < n; i++) { ib[i] = idx[i]; }
        sprBmp[k] = bmp; sprIdx[k] = ib; sprPal[k] = plist; sprKey[k] = 0;
    }

    function initWeaponBitmap(f) {
        if (!(Graphics has :createBufferedBitmap)) { wpnBmp = null; return; }
        if (wpnBmp == null) { wpnBmp = new [NWPN]; }
        var src = pal[0];
        /* palette: transparent + every colour this frame uses */
        var used = {};
        var plist = [Graphics.COLOR_TRANSPARENT];
        var fb = f * WPN_W * WPN_H;
        var top = wpnTop[f]; var bot = wpnBot[f];
        for (var cx = 0; cx < WPN_W; cx++) {
            var cb = fb + cx * WPN_H;
            for (var y = top[cx]; y < bot[cx]; y++) {
                var c = wGfx[cb + y];
                if (c != 0 && !used.hasKey(c)) { used.put(c, true); plist.add(src[c]); }
            }
        }
        var ref = Graphics.createBufferedBitmap({ :width => WPN_W, :height => WPN_H, :palette => plist });
        var bmp = ref.get();
        var bdc = bmp.getDc();
        for (var cx = 0; cx < WPN_W; cx++) {
            var a = top[cx]; var b = bot[cx];
            var cb = fb + cx * WPN_H;
            var y = a;
            while (y < b) {
                var c = wGfx[cb + y];
                var y1 = y + 1;
                while (y1 < b && wGfx[cb + y1] == c) { y1++; }
                if (c != 0) {
                    bdc.setColor(src[c], Graphics.COLOR_TRANSPARENT);
                    bdc.fillRectangle(cx, y, 1, y1 - y);
                }
                y = y1;
            }
        }
        wpnBmp[f] = ref;
    }

    function initWorld(unused) {
        for (var y = 0; y < MAPH; y++) {
            var row = mapSrc[y].toCharArray();
            for (var x = 0; x < MAPW; x++) {
                var c = row[x].toNumber();
                var i = y * MAPW + x;
                world[i] = (c >= 49 && c <= 52) ? (c - 48) : 0;           /* '1'..'4' */
                doorIdx[i] = 0;
                hmap[i] = 0;
            }
        }
        ndoor = 0;
        for (var y = 0; y < MAPH; y++) {
            var hrow = hSrc[y].toCharArray();
            for (var x = 0; x < MAPW; x++) {
                var h = hrow[x].toNumber();
                var i = y * MAPW + x;
                var fh = 0; var ch = 8;
                if (h >= 49 && h <= 53)      { fh = h - 48; }              /* '1'..'5' */
                else if (h >= 97 && h <= 101) { ch = 8 - (h - 97 + 1); }   /* 'a'..'e' */
                else if (h == 68 && world[i] == 0 && ndoor < MAXDOOR) {    /* 'D' */
                    doorMx[ndoor] = x; doorMy[ndoor] = y;
                    doorOpen[ndoor] = 0; doorWant[ndoor] = 0; doorHold[ndoor] = 0;
                    ndoor++;
                    doorIdx[i] = ndoor;
                }
                hmap[i] = (fh << 4) | ch;
            }
        }

        for (var i = 0; i < MAPW * MAPH; i++) {
            if (world[i] != 0)        { cellKey[i] = KEY_WALL; }
            else if (doorIdx[i] != 0) { cellKey[i] = KEY_DOOR + doorIdx[i] - 1; }
            else                      { cellKey[i] = hmap[i]; }
        }

        px = 2 * ONE + ONE / 2;   /* cell (2,2): open on all sides */
        py = 2 * ONE + ONE / 2;
        pa = 0;

        var place = [ [12,4,0], [17,8,0], [6,14,0], [21,19,1], [11,11,0],
                      [5,19,0], [8,7,1], [9,8,0], [15,16,0], [18,3,1] ];
        nact = place.size();
        for (var i = 0; i < MAXACT; i++) { aSt[i] = A_GONE; }
        for (var i = 0; i < nact; i++) {
            var p = place[i];
            aX[i] = p[0] * ONE + ONE / 2;
            aY[i] = p[1] * ONE + ONE / 2;
            aType[i] = p[2];
            aHp[i] = (p[2] != 0) ? 60 : 30;
            aSt[i] = A_IDLE;
            aDir[i] = (i * 349) & NA_MASK;
            aFrame[i] = 0; aTics[i] = 0; aAtk[i] = 0; aWob[i] = 0; aDist[i] = 0;
        }
        var pk = [ [4,2,P_AMMO], [20,2,P_HEALTH], [13,7,P_AMMO], [3,11,P_HEALTH],
                   [21,11,P_AMMO], [12,13,P_HEALTH], [2,17,P_AMMO], [18,17,P_HEALTH],
                   [6,21,P_AMMO], [15,22,P_HEALTH] ];
        npick = pk.size();
        for (var i = 0; i < npick; i++) {
            pX[i] = pk[i][0] * ONE + ONE / 2;
            pY[i] = pk[i][1] * ONE + ONE / 2;
            pType[i] = pk[i][2];
            pOn[i] = 1; pDist[i] = 0;
        }
        for (var i = 0; i < MAXPROJ; i++) { jOn[i] = 0; }

        health = 100; ammo = 50; kills = 0;
        viewCy = VIEW_CY; deadTic = 0;
        wpnTic = 0; wpnFrame = 0; bobPhase = 0; bobX = 0; bobY = 0;
        flashTic = 0; painTic = 0;
        for (var i = 0; i < 9; i++) { burst[i] = 0; }
        held = ACT_NONE; wantFire = false; wantUse = false; wantRestart = false;
        eyeZ = floorz(px >> FIX, py >> FIX) + EYE_H;
    }

    /* ------------------------------------------------------------------
     * world queries
     * ------------------------------------------------------------------ */

    function floorz(mx, my) {
        return (hmap[my * MAPW + mx] & 0xF0) << 1;
    }

    function ceilz(mx, my) {
        var i = my * MAPW + mx;
        var h = hmap[i];
        var base = (h & 15) << 5;
        var d = doorIdx[i];
        if (d != 0) {
            var f = (h & 0xF0) << 1;
            return f + ((base - f) * doorOpen[d - 1]) / 64;
        }
        return base;
    }

    function pointAngle(dx, dy) {
        var ax = dx < 0 ? -dx : dx;
        var ay = dy < 0 ? -dy : dy;
        var a;
        if (ax == 0 && ay == 0) { return 0; }
        if (ax >= ay) { a = atanTab[(ay << 6) / ax]; }
        else          { a = NA_90 - atanTab[(ax << 6) / ay]; }
        if (dx < 0) { a = (NA / 2) - a; }
        if (dy < 0) { a = -a; }
        return a & NA_MASK;
    }

    function solidAt(fx, fy) {
        var mx = fx >> FIX; var my = fy >> FIX;
        if (mx < 0 || mx >= MAPW || my < 0 || my >= MAPH) { return true; }
        return world[my * MAPW + mx] != 0;
    }

    /* coarse line of sight: step ~1/4 cell and test for walls */
    function los(x0, y0, x1, y1) {
        var dx = x1 - x0; var dy = y1 - y0;
        var ax = dx < 0 ? -dx : dx; var ay = dy < 0 ? -dy : dy;
        var n = (ax > ay ? ax : ay) >> 6;
        if (n < 2) { return true; }
        if (n > 72) { n = 72; }
        var sx = dx / n; var sy = dy / n;
        var x = x0; var y = y0;
        for (var i = 1; i < n; i++) {
            x += sx; y += sy;
            if (solidAt(x, y)) { return false; }
        }
        return true;
    }

    /* ------------------------------------------------------------------
     * drawing primitive: one run.  Skips setColor when the colour did not
     * change - runs from the same texel ramp often repeat.
     * ------------------------------------------------------------------ */

    function fillRun(dc, x, y, w, h, c) {
        if (c != mLastCol) { dc.setColor(c, Graphics.COLOR_TRANSPARENT); mLastCol = c; }
        dc.fillRectangle(x, y, w, h);
        mCalls++;
    }

    /* One textured wall face.  yTopF/yBotF are where the face's top and
     * bottom edges land unclipped; y0/y1 are the part we may draw.
     *
     * Walks TEXELS, not rows: texel k covers rows
     *   [yTopF + k*P/texels, yTopF + (k+1)*P/texels)
     * so a 400-row face costs at most 32 iterations, and consecutive
     * texels of the same colour collapse into one fillRectangle. */
    /* One textured face: a single clipped drawScaledBitmap.  The whole
     * 64-column texture is scaled so that column tcol lands on x, and the
     * clip keeps only this ray's XSTEP-wide strip between y0 and y1.
     * Faces are at most one cell (32 texels) tall, so no tiling. */
    function drawFace(dc, x, y0, y1, yTopF, yBotF, dz, bmp, tcol) {
        if (y0 < 0) { y0 = 0; }
        if (y1 > VIEW_H) { y1 = VIEW_H; }
        var P = yBotF - yTopF;
        if (y0 >= y1 || P <= 0) { return; }
        var texels = dz >> 3;
        if (texels < 1) { texels = 1; }
        dc.setClip(x, y0, XSTEP, y1 - y0);
        dc.drawScaledBitmap(x - tcol * XSTEP, yTopF, TEXW * XSTEP, (P * TEXH) / texels, bmp);
        dc.clearClip();
        mCalls += 3;
    }

    /* ------------------------------------------------------------------
     * wall casting - the core loop.  Does not stop at the first wall: it
     * keeps stepping outward, narrowing a per-column clip window, drawing
     * a face wherever the floor rises or the ceiling drops.
     *
     * The map border is solid, so there is no bounds check, and cells are
     * compared by a precomputed key so the common "nothing changed" step
     * is one ByteArray read and one compare.
     * ------------------------------------------------------------------ */

    function renderWalls(dc) {
        var cosT = cosTab; var icosT = icosTab; var cAng = colAng; var cCos = colCos;
        var key = cellKey; var wld = world; var hm = hmap; var dOpen = doorOpen;
        var zb = zbuf; var tb = texBmp; var sRGB = shadeRGB;
        var pxx = px; var pyy = py; var paa = pa; var eye = eyeZ; var vcy = viewCy;
        var pScale = projScale;
        var mapX0 = pxx >> FIX; var mapY0 = pyy >> FIX;
        var fracX = pxx & 255; var fracY = pyy & 255;
        var ci0 = mapY0 * MAPW + mapX0;
        var pk0 = key[ci0];
        var pf0 = floorz(mapX0, mapY0);
        var pc0 = ceilz(mapX0, mapY0);
        var bgC = sRGB[3][COL_CEIL]; var bgF = sRGB[3][COL_FLOOR];
        var lc = -1;
        var TR = Graphics.COLOR_TRANSPARENT;
        var calls = 0;

        for (var r = 0; r < NRAY; r++) {
            var x = r << XSHIFT;
            var ang = paa + cAng[r];
            if (ang < 0) { ang += NA; } else if (ang >= NA) { ang -= NA; }
            var sa = ang - NA_90;
            if (sa < 0) { sa += NA; }
            var cs = cosT[ang]; var sn = cosT[sa];
            var ddx = icosT[ang]; var ddy = icosT[sa];
            var stepX; var stepYW; var sdx; var sdy; var side = 0;
            var ytop = 0; var ybot = VIEW_H;
            var pf = pf0; var pc = pc0; var pk = pk0; var ci = ci0;
            var guard = GUARD;

            zb[r] = DIST_MAX;

            if (cs < 0) { stepX = -1;    sdx = (fracX * ddx) >> FIX; }
            else        { stepX =  1;    sdx = ((ONE - fracX) * ddx) >> FIX; }
            if (sn < 0) { stepYW = -MAPW; sdy = (fracY * ddy) >> FIX; }
            else        { stepYW =  MAPW; sdy = ((ONE - fracY) * ddy) >> FIX; }

            while (guard > 0) {
                guard--; mIters++;
                if (sdx < sdy) { sdx += ddx; ci += stepX;  side = 0; }
                else           { sdy += ddy; ci += stepYW; side = 1; }

                var k = key[ci];
                if (k == pk) { continue; }                /* same heights: nothing to draw */
                pk = k;

                var tid = 0; var f = pf; var c = pc;
                if (k == KEY_WALL) {
                    tid = wld[ci];
                } else {
                    var h = hm[ci];
                    f = (h & 0xF0) << 1;
                    c = (h & 15) << 5;
                    if (k >= KEY_DOOR) { c = f + ((c - f) * dOpen[k - KEY_DOOR]) / 64; }
                }

                var dist = (side == 0) ? sdx - ddx : sdy - ddy;
                if (dist < DIST_MIN) { dist = DIST_MIN; } else if (dist > DIST_MAX) { dist = DIST_MAX; }
                var pdist = (dist * cCos[r]) >> FIX;
                if (pdist < DIST_MIN) { pdist = DIST_MIN; }

                var scale = pScale / pdist;
                var lv = pdist >> 10;
                if (side != 0) { lv += 1; }
                if (lv > 7) { lv = 7; }
                var lut = sRGB[lv];

                /* where the cell we are leaving projects at this boundary */
                var ypf = vcy + (((eye - pf) * scale) >> FIX);
                var ypc = vcy + (((eye - pc) * scale) >> FIX);

                /* its floor and ceiling spans, flat-shaded by distance */
                if (ypf < ybot) {
                    var t = ypf > ytop ? ypf : ytop;
                    if (ybot > t) {
                        var col = lut[COL_FLOOR];
                        if (col != lc) { dc.setColor(col, TR); lc = col; }
                        dc.fillRectangle(x, t, XSTEP, ybot - t); calls++;
                    }
                    ybot = t;
                }
                if (ypc > ytop) {
                    var t2 = ypc < ybot ? ypc : ybot;
                    if (t2 > ytop) {
                        var col2 = lut[COL_CEIL];
                        if (col2 != lc) { dc.setColor(col2, TR); lc = col2; }
                        dc.fillRectangle(x, ytop, XSTEP, t2 - ytop); calls++;
                    }
                    ytop = t2;
                }
                if (ytop >= ybot) { break; }

                /* the texture column is the same for every face in this cell */
                var wallx;
                if (side == 0) {
                    wallx = (pyy + ((dist * sn) >> FIX)) & 255;
                    if (stepX > 0) { wallx = 255 - wallx; }
                } else {
                    wallx = (pxx + ((dist * cs) >> FIX)) & 255;
                    if (stepYW < 0) { wallx = 255 - wallx; }
                }
                var tcol = wallx >> 2;

                if (tid != 0) {                                   /* solid block */
                    drawFace(dc, x, ytop, ybot, ypc, ypf, pc - pf, tb[((tid - 1) << 3) + lv], tcol);
                    zb[r] = pdist;
                    ytop = ybot;
                    break;
                }

                if (f > pf) {                                     /* floor steps up */
                    var yf = vcy + (((eye - f) * scale) >> FIX);
                    if (yf < ybot) {
                        var t3 = yf > ytop ? yf : ytop;
                        drawFace(dc, x, t3, ybot, yf, ypf, f - pf, tb[8 + lv], tcol);
                        ybot = t3;
                    }
                }
                if (c < pc) {                                     /* ceiling drops */
                    var yc = vcy + (((eye - c) * scale) >> FIX);
                    if (yc > ytop) {
                        var t4 = yc < ybot ? yc : ybot;
                        drawFace(dc, x, ytop, t4, ypc, yc, pc - c, tb[16 + lv], tcol);
                        ytop = t4;
                    }
                }
                pf = f; pc = c;
                if (ytop >= ybot) { break; }
            }

            if (ytop < ybot) {                                    /* open sky / void */
                var mid = vcy;
                if (mid < ytop) { mid = ytop; }
                if (mid > ybot) { mid = ybot; }
                if (mid > ytop) { if (bgC != lc) { dc.setColor(bgC, TR); lc = bgC; } dc.fillRectangle(x, ytop, XSTEP, mid - ytop); calls++; }
                if (ybot > mid) { if (bgF != lc) { dc.setColor(bgF, TR); lc = bgF; } dc.fillRectangle(x, mid, XSTEP, ybot - mid); calls++; }
            }
        }
        mLastCol = lc;
        mCalls += calls;
    }

    /* ------------------------------------------------------------------
     * billboards: actors, pickups, projectiles.  One prebuilt palette
     * bitmap per frame; shading is a setPalette; wall occlusion is done
     * per run of visible rays with a clip, so a sprite costs a handful of
     * calls however big it is.
     * ------------------------------------------------------------------ */

    function drawBillboard(dc, fwd, rgt, wz, worldH, k, gwid, ghei, lv, type) {
        var scale = projScale / fwd;
        var vcy = viewCy; var eye = eyeZ;
        var yb = vcy + (((eye - wz) * scale) >> FIX);
        var yt = vcy + (((eye - (wz + worldH)) * scale) >> FIX);
        var h = yb - yt;
        if (h < 2) { return; }
        var w = (h * gwid) / ghei;
        if (w < 2) { return; }
        var sx = SCR_W / 2 + (rgt * projd) / fwd;
        var x0 = sx - (w >> 1); var x1 = x0 + w;
        if (x1 <= 0 || x0 >= SCR_W || yb <= 0 || yt >= VIEW_H) { return; }
        var ys = yt < 0 ? 0 : yt;
        var ye = yb > VIEW_H ? VIEW_H : yb;

        /* shade: run the frame's colour bytes through the table */
        var bmp = sprBmp[k];
        var skey = (lv << 1) | type;
        if (sprKey[k] != skey) {
            var lut = (type != 0) ? shadeERGB[lv] : shadeRGB[lv];
            var idx = sprIdx[k]; var pa = sprPal[k];
            var n = idx.size();
            for (var i = 1; i < n; i++) { pa[i] = lut[idx[i]]; }
            bmp.setPalette(pa);
            sprKey[k] = skey;
            mCalls++;
        }

        /* draw once per run of rays not hidden by a nearer wall */
        var zb = zbuf;
        var r0 = x0 < 0 ? 0 : x0 / XSTEP;
        var r1 = (x1 - 1) / XSTEP;
        if (r1 >= NRAY) { r1 = NRAY - 1; }
        var start = -1;
        for (var r = r0; r <= r1; r++) {
            var vis = fwd < zb[r];
            if (vis && start < 0) { start = r; }
            if (start >= 0 && (!vis || r == r1)) {
                var rx0 = start * XSTEP;
                var rx1 = (vis ? r + 1 : r) * XSTEP;
                dc.setClip(rx0, ys, rx1 - rx0, ye - ys);
                dc.drawScaledBitmap(x0, yt, w, h, bmp);
                mCalls += 2;
                start = -1;
            }
        }
        dc.clearClip();
    }

    /* gather actors, pickups and projectiles, sort far to near, draw */
    function renderThings(dc) {
        var n = 0;
        var cs = cosTab[pa]; var sn = cosTab[(pa - NA_90) & NA_MASK];
        var pxx = px; var pyy = py;
        var rd = rDist; var rk = rKind; var ri = rIdx;

        for (var i = 0; i < nact; i++) {
            if (aSt[i] == A_GONE) { continue; }
            var fwd = ((aX[i] - pxx) * cs + (aY[i] - pyy) * sn) >> FIX;
            if (fwd < 96) { continue; }
            aDist[i] = fwd;
            rd[n] = fwd; rk[n] = K_ACT; ri[n] = i; n++;
        }
        for (var i = 0; i < npick; i++) {
            if (pOn[i] == 0) { continue; }
            var fwd = ((pX[i] - pxx) * cs + (pY[i] - pyy) * sn) >> FIX;
            if (fwd < 96) { continue; }
            pDist[i] = fwd;
            rd[n] = fwd; rk[n] = K_PICK; ri[n] = i; n++;
        }
        for (var i = 0; i < MAXPROJ; i++) {
            if (jOn[i] == 0) { continue; }
            var fwd = ((jX[i] - pxx) * cs + (jY[i] - pyy) * sn) >> FIX;
            if (fwd < 64) { continue; }
            jDist[i] = fwd;
            rd[n] = fwd; rk[n] = K_PROJ; ri[n] = i; n++;
        }

        for (var i = 1; i < n; i++) {                              /* far to near */
            var vd = rd[i]; var vk = rk[i]; var vi = ri[i];
            var j = i - 1;
            while (j >= 0 && rd[j] < vd) { rd[j + 1] = rd[j]; rk[j + 1] = rk[j]; ri[j + 1] = ri[j]; j--; }
            rd[j + 1] = vd; rk[j + 1] = vk; ri[j + 1] = vi;
        }

        for (var q = 0; q < n; q++) {
            var fwd = rd[q]; var i = ri[q]; var kind = rk[q];
            var dx; var dy; var rgt; var lv;
            if (kind == K_ACT) {
                var frame; var mirror = 0;
                dx = aX[i] - pxx; dy = aY[i] - pyy;
                rgt = (-dx * sn + dy * cs) >> FIX;
                lv = fwd >> 10; if (lv > 7) { lv = 7; }
                var st = aSt[i];
                if (st == A_FIRE) { lv = 0; }
                if (st == A_DIE || st == A_DEAD) {
                    frame = F_DIE + aFrame[i];
                } else {
                    var ang = pointAngle(dx, dy);
                    var d   = (ang - aDir[i] + NA / 2) & NA_MASK;
                    var r8  = ((d + NA / 16) & NA_MASK) >> 8;
                    var rot = rotMap[r8];
                    mirror  = rotMirr[r8];
                    frame = (st == A_FIRE) ? F_FIRE + rot : F_WALK + rot * 2 + (aFrame[i] & 1);
                }
                drawBillboard(dc, fwd, rgt, floorz(aX[i] >> FIX, aY[i] >> FIX), ONE,
                              sprMap[frame * 2 + mirror], SPR_W, SPR_H, lv, aType[i] != 0 ? 1 : 0);
            } else if (kind == K_PICK) {
                dx = pX[i] - pxx; dy = pY[i] - pyy;
                rgt = (-dx * sn + dy * cs) >> FIX;
                lv = fwd >> 10; if (lv > 7) { lv = 7; }
                drawBillboard(dc, fwd, rgt, floorz(pX[i] >> FIX, pY[i] >> FIX), ONE * 2 / 5,
                              PK0 + pType[i], PK_W, PK_H, lv, 0);
            } else {
                dx = jX[i] - pxx; dy = jY[i] - pyy;
                rgt = (-dx * sn + dy * cs) >> FIX;
                drawBillboard(dc, fwd, rgt, jZ[i], ONE / 4,
                              PJ0 + jFrame[i], PJ_W, PJ_H, 0, 0);
            }
        }
    }

    /* ------------------------------------------------------------------
     * weapon, crosshair, HUD
     * ------------------------------------------------------------------ */

    function renderWeapon(dc) {
        var ox = (SCR_W - WPN_W * WPN_S) / 2 + bobX * 2;
        var oy = VIEW_H - WPN_H * WPN_S + bobY * 2;
        if (wpnBmp != null) {
            var bm = wpnBmp[wpnFrame].get();
            dc.drawScaledBitmap(ox, oy, WPN_W * WPN_S, WPN_H * WPN_S, bm);
            mCalls++;
            return;
        }
        /* fallback: runs straight from the sprite */
        var src = pal[0];
        var fb = wpnFrame * WPN_W * WPN_H;
        var top = wpnTop[wpnFrame]; var bot = wpnBot[wpnFrame];
        for (var cx = 0; cx < WPN_W; cx++) {
            var a = top[cx]; var b = bot[cx];
            var cb = fb + cx * WPN_H;
            var y = a;
            while (y < b) {
                var c = wGfx[cb + y];
                var y1 = y + 1;
                while (y1 < b && wGfx[cb + y1] == c) { y1++; }
                if (c != 0) {
                    var sy = oy + y * WPN_S; var sh = (y1 - y) * WPN_S;
                    if (sy + sh > VIEW_H) { sh = VIEW_H - sy; }
                    if (sh > 0) { fillRun(dc, ox + cx * WPN_S, sy, WPN_S, sh, src[c]); }
                }
                y = y1;
            }
        }
    }

    function renderCrosshair(dc) {
        var c = pal[0][COL_XHAIR];
        var cx = SCR_W / 2; var cy = viewCy;
        fillRun(dc, cx - 14, cy - 1, 9, 3, c);
        fillRun(dc, cx + 6,  cy - 1, 9, 3, c);
        fillRun(dc, cx - 1, cy - 14, 3, 9, c);
        fillRun(dc, cx - 1, cy + 6,  3, 9, c);
    }

    function bar(dc, x, y, w, h, pct, fg, bg) {
        if (pct > 100) { pct = 100; } else if (pct < 0) { pct = 0; }
        var fill = (w * pct) / 100;
        if (fill > 0) { fillRun(dc, x, y, fill, h, fg); }
        if (fill < w) { fillRun(dc, x + fill, y, w - fill, h, bg); }
    }

    function renderHud(dc) {
        var p = pal[0];
        var hy = VIEW_H;
        fillRun(dc, 0, hy, SCR_W, SCR_H - hy, p[(2 << 5) | 5]);
        fillRun(dc, 0, hy, SCR_W, 2, p[(2 << 5) | 11]);
        var font = Graphics.FONT_SMALL;
        var ty = hy + 6;                        /* number row */
        var by = hy + 46;                       /* bar row    */

        /* health: red cross, number, bar */
        fillRun(dc, 30, ty + 6, 4, 14, p[(5 << 5) | 28]);
        fillRun(dc, 25, ty + 11, 14, 4, p[(5 << 5) | 28]);
        dc.setColor(p[(5 << 5) | 30], Graphics.COLOR_TRANSPARENT); mLastCol = -1;
        dc.drawText(48, ty, font, health.format("%d"), Graphics.TEXT_JUSTIFY_LEFT);
        bar(dc, 24, by, 130, 8, health, p[(5 << 5) | 26], p[(2 << 5) | 8]);

        /* ammo: brass round, number, bar */
        fillRun(dc, 182, ty + 4, 2, 18, p[(6 << 5) | 24]);
        fillRun(dc, 184, ty + 4, 2, 18, p[(6 << 5) | 28]);
        fillRun(dc, 186, ty + 4, 2, 18, p[(6 << 5) | 20]);
        dc.setColor(p[(6 << 5) | 30], Graphics.COLOR_TRANSPARENT); mLastCol = -1;
        dc.drawText(198, ty, font, ammo.format("%d"), Graphics.TEXT_JUSTIFY_LEFT);
        bar(dc, 176, by, 130, 8, ammo * 2, p[(6 << 5) | 24], p[(2 << 5) | 8]);

        /* kills */
        fillRun(dc, 330, ty + 12, 10, 4, p[20]);
        dc.setColor(p[26], Graphics.COLOR_TRANSPARENT); mLastCol = -1;
        dc.drawText(348, ty, font, kills.format("%d"), Graphics.TEXT_JUSTIFY_LEFT);

        /* fps + draw calls, small, bottom right */
        dc.setColor(p[(3 << 5) | 28], Graphics.COLOR_TRANSPARENT); mLastCol = -1;
        dc.drawText(SCR_W - 44, by + 10, Graphics.FONT_XTINY,
                    curFps.format("%d") + "fps " + frameCalls.format("%d") + "c",
                    Graphics.TEXT_JUSTIFY_RIGHT);
    }

    function renderLoading(dc) {
        dc.setColor(Graphics.COLOR_BLACK, Graphics.COLOR_BLACK);
        dc.clear();
        if (logo == null) { logo = WatchUi.loadResource(Rez.Drawables.Logo); }
        var lw = logo.getWidth(); var lh = logo.getHeight();
        dc.drawBitmap((SCR_W - lw) / 2, SCR_H / 2 - lh - 20, logo);
        dc.setColor(0x9AA0A8, Graphics.COLOR_TRANSPARENT);
        dc.drawText(SCR_W / 2, SCR_H / 2 + 6, Graphics.FONT_SMALL, "generating art...", Graphics.TEXT_JUSTIFY_CENTER);
        var w = taskN > 0 ? (SCR_W - 120) * initStep / taskN : 0;
        dc.setColor(0x401410, Graphics.COLOR_TRANSPARENT);
        dc.fillRectangle(60, SCR_H / 2 + 56, SCR_W - 120, 8);
        dc.setColor(0xD74614, Graphics.COLOR_TRANSPARENT);
        dc.fillRectangle(60, SCR_H / 2 + 56, w, 8);
    }

    /* ------------------------------------------------------------------
     * movement / doors
     * ------------------------------------------------------------------ */

    /* can the player stand here, coming from a floor at `from`? */
    function canEnter(fx, fy, from) {
        var mx = fx >> FIX; var my = fy >> FIX;
        if (mx < 0 || mx >= MAPW || my < 0 || my >= MAPH) { return false; }
        if (world[my * MAPW + mx] != 0) { return false; }
        var f = floorz(mx, my);
        var c = ceilz(mx, my);
        if (f - from > MAXSTEP) { return false; }     /* ledge too high        */
        if (c - f < HEADROOM)   { return false; }     /* shut door, crawlspace */
        return true;
    }

    function move(dx, dy) {
        var r = 56;                                   /* player radius */
        var nx = px + dx; var ny = py + dy;
        var fz = floorz(px >> FIX, py >> FIX);
        if (canEnter(nx + (dx > 0 ? r : -r), py, fz)) { px = nx; }
        fz = floorz(px >> FIX, py >> FIX);
        if (canEnter(px, ny + (dy > 0 ? r : -r), fz)) { py = ny; }
    }

    function cellOccupied(mx, my) {
        if ((px >> FIX) == mx && (py >> FIX) == my) { return true; }
        for (var i = 0; i < nact; i++) {
            if (aSt[i] != A_DEAD && aSt[i] != A_GONE &&
                (aX[i] >> FIX) == mx && (aY[i] >> FIX) == my) { return true; }
        }
        return false;
    }

    function updateDoors() {
        for (var i = 0; i < ndoor; i++) {
            if (doorHold[i] > 0) {
                doorHold[i]--;
                if (doorHold[i] == 0) { doorWant[i] = 0; }
            }
            if (doorOpen[i] < doorWant[i]) {
                doorOpen[i] += 4;
                if (doorOpen[i] > 64) { doorOpen[i] = 64; }
            } else if (doorOpen[i] > doorWant[i]) {
                if (cellOccupied(doorMx[i], doorMy[i])) { doorHold[i] = 30; }
                else if (doorOpen[i] >= 4) { doorOpen[i] -= 4; }
                else { doorOpen[i] = 0; }
            }
        }
    }

    /* open whatever door the player is facing */
    function useDoor() {
        var cs = cosTab[pa]; var sn = cosTab[(pa - NA_90) & NA_MASK];
        for (var step = ONE / 2; step <= ONE * 3 / 2; step += ONE / 2) {
            var mx = (px + ((cs * step) >> FIX)) >> FIX;
            var my = (py + ((sn * step) >> FIX)) >> FIX;
            if (mx < 0 || mx >= MAPW || my < 0 || my >= MAPH) { return; }
            var d = doorIdx[my * MAPW + mx];
            if (d != 0) {
                doorWant[d - 1] = 64;
                doorHold[d - 1] = 150;
                return;
            }
        }
    }

    /* ------------------------------------------------------------------
     * combat
     * ------------------------------------------------------------------ */

    function hurtActor(i, dmg) {
        if (aSt[i] == A_DIE || aSt[i] == A_DEAD) { return; }
        aHp[i] -= dmg;
        if (aHp[i] <= 0) {
            aSt[i] = A_DIE; aFrame[i] = 0; aTics[i] = 0;
            kills++;
        } else {
            aSt[i] = A_PAIN; aTics[i] = 4;
        }
    }

    function playerFire() {
        if (ammo <= 0 || wpnTic != 0 || deadTic != 0) { return; }
        ammo--;
        wpnTic = 10;
        flashTic = 2;
        var cs = cosTab[pa]; var sn = cosTab[(pa - NA_90) & NA_MASK];
        var best = -1; var bestd = 1 << 20;
        for (var i = 0; i < nact; i++) {
            var st = aSt[i];
            if (st == A_DIE || st == A_DEAD || st == A_GONE) { continue; }
            var dx = aX[i] - px; var dy = aY[i] - py;
            var fwd = (dx * cs + dy * sn) >> FIX;
            if (fwd < 64) { continue; }
            var rgt = (-dx * sn + dy * cs) >> FIX;
            if (rgt < 0) { rgt = -rgt; }
            if (rgt * 9 > fwd) { continue; }          /* ~6 degree cone */
            if (fwd >= bestd) { continue; }
            if (!los(px, py, aX[i], aY[i])) { continue; }
            bestd = fwd; best = i;
        }
        if (best >= 0) { hurtActor(best, 22 + (rnd() & 15)); }
    }

    function hurtPlayer(dmg) {
        if (deadTic != 0) { return; }
        health -= dmg;
        painTic = 3;
        if (health <= 0) { health = 0; deadTic = 1; }
    }

    function actorMove(i, dx, dy) {
        var r = 48;
        var nx = aX[i] + dx; var ny = aY[i] + dy;
        if (!solidAt(nx + (dx > 0 ? r : -r), aY[i])) { aX[i] = nx; }
        if (!solidAt(aX[i], ny + (dy > 0 ? r : -r))) { aY[i] = ny; }
    }

    function spawnProj(i) {
        for (var j = 0; j < MAXPROJ; j++) {
            if (jOn[j] != 0) { continue; }
            var ang = pointAngle(px - aX[i], py - aY[i]);
            var cs = cosTab[ang]; var sn = cosTab[(ang - NA_90) & NA_MASK];
            jX[j] = aX[i] + ((cs * 40) >> FIX);
            jY[j] = aY[i] + ((sn * 40) >> FIX);
            jZ[j] = floorz(aX[i] >> FIX, aY[i] >> FIX) + ONE / 2;
            jVx[j] = (cs * PROJ_SPD) >> FIX;
            jVy[j] = (sn * PROJ_SPD) >> FIX;
            jOn[j] = 1; jLife[j] = 250; jFrame[j] = 0;
            return;
        }
    }

    function updateProjectiles() {
        for (var i = 0; i < MAXPROJ; i++) {
            if (jOn[i] == 0) { continue; }
            jX[i] += jVx[i];
            jY[i] += jVy[i];
            jFrame[i] = jFrame[i] ^ 1;
            jLife[i]--;
            if (jLife[i] == 0) { jOn[i] = 0; continue; }
            var mx = jX[i] >> FIX; var my = jY[i] >> FIX;
            if (mx < 0 || mx >= MAPW || my < 0 || my >= MAPH) { jOn[i] = 0; continue; }
            if (world[my * MAPW + mx] != 0) { jOn[i] = 0; continue; }
            if (jZ[i] < floorz(mx, my) || jZ[i] > ceilz(mx, my)) { jOn[i] = 0; continue; }
            var dx = jX[i] - px; var dy = jY[i] - py;
            if (dx < 0) { dx = -dx; }
            if (dy < 0) { dy = -dy; }
            if (dx < 96 && dy < 96) {                 /* hit the player */
                jOn[i] = 0;
                hurtPlayer(7 + (rnd() & 7));
            }
        }
    }

    function checkPickups() {
        for (var i = 0; i < npick; i++) {
            if (pOn[i] == 0) { continue; }
            var dx = pX[i] - px; var dy = pY[i] - py;
            if (dx < 0) { dx = -dx; }
            if (dy < 0) { dy = -dy; }
            if (dx > 110 || dy > 110) { continue; }
            if (pType[i] == P_HEALTH) {
                if (health >= 100) { continue; }
                health += 25;
                if (health > 100) { health = 100; }
            } else {
                if (ammo >= 99) { continue; }
                ammo += 20;
                if (ammo > 99) { ammo = 99; }
            }
            pOn[i] = 0;
            flashTic = 1;
        }
    }

    function updateActors() {
        for (var i = 0; i < nact; i++) {
            var st = aSt[i];
            if (st == A_GONE) { continue; }
            if (st == A_DEAD) {                       /* linger, then fade */
                aTics[i]++;
                if (aTics[i] >= CORPSE_LIFE) { aSt[i] = A_GONE; }
                continue;
            }
            if (st == A_DIE) {                        /* death animation */
                aTics[i]++;
                if (aTics[i] >= 8) {
                    aTics[i] = 0;
                    aFrame[i]++;
                    if (aFrame[i] >= 4) { aFrame[i] = 4; aSt[i] = A_DEAD; aTics[i] = 0; }
                }
                continue;
            }

            var dx = px - aX[i]; var dy = py - aY[i];
            var adx = dx < 0 ? -dx : dx; var ady = dy < 0 ? -dy : dy;
            var adist = (adx > ady) ? adx + (ady >> 1) : ady + (adx >> 1);
            var see = deadTic == 0 && adist < 16 * ONE && los(aX[i], aY[i], px, py);

            if (st == A_IDLE) {
                if (see) { aSt[i] = A_CHASE; }
                continue;
            }
            if (st == A_PAIN) {
                aTics[i]--;
                if (aTics[i] == 0) { aSt[i] = A_CHASE; }
                continue;
            }
            if (st == A_FIRE) {
                aTics[i]--;
                if (aTics[i] == 0) {
                    if (see && adist < 13 * ONE) { spawnProj(i); }
                    aSt[i] = A_CHASE;
                    aAtk[i] = 40 + (rnd() & 31);
                }
                continue;
            }

            /* chase: face the player, drift a little so you see other angles */
            if ((rnd() & 63) == 0) { aWob[i] = (rnd() % 3) - 1; }
            aDir[i] = (pointAngle(dx, dy) + aWob[i] * (NA / 16)) & NA_MASK;

            if (adist > 2 * ONE) {
                var sp = (aType[i] != 0) ? 2 : 3;
                var d = aDir[i];
                actorMove(i, (cosTab[d] * sp) >> FIX, (cosTab[(d - NA_90) & NA_MASK] * sp) >> FIX);
            }
            aTics[i]++;
            if (aTics[i] >= 12) { aTics[i] = 0; aFrame[i] = aFrame[i] ^ 1; }

            if (aAtk[i] > 0) { aAtk[i]--; }
            else if (see && adist < 11 * ONE && (rnd() & 63) == 0) {
                aSt[i] = A_FIRE; aTics[i] = 7;
            }
        }
    }

    /* ------------------------------------------------------------------
     * input, from the delegate
     * ------------------------------------------------------------------ */

    function action(a, frames) {
        if (deadTic >= 40) { wantRestart = true; return; }
        if (a == ACT_FIRE) { wantFire = true; return; }
        if (a == ACT_USE)  { wantUse = true; return; }
        if (a != ACT_NONE) { burst[a] += frames; if (burst[a] > 40) { burst[a] = 40; } }
    }

    function hold(a) {
        if (a == ACT_FIRE || a == ACT_USE) { action(a, 0); return; }
        held = a;
    }

    function release() { held = ACT_NONE; }

    function active(a) {
        if (held == a) { return true; }
        if (burst[a] > 0) { burst[a]--; return true; }
        return false;
    }

    /* ------------------------------------------------------------------
     * one frame: logic then render
     * ------------------------------------------------------------------ */

    /* Logic runs from the timer callback, rendering from onUpdate: two
     * separate callbacks, so each gets its own watchdog budget. */
    function tick() {
        if (!initDone()) { initChunk(); return; }

        var moving = false;

        if (wantRestart) { initWorld(0); }
        if (deadTic != 0) {
            if (deadTic < 40) { deadTic++; }
            if (viewCy > 80) { viewCy -= 6; }        /* camera drops to the floor */
        } else {
            var SPD = moveSpeed; var TURN = turnSpeed;
            if (active(ACT_LEFT))  { pa = (pa - TURN) & NA_MASK; }
            if (active(ACT_RIGHT)) { pa = (pa + TURN) & NA_MASK; }
            var cs = cosTab[pa]; var sn = cosTab[(pa - NA_90) & NA_MASK];
            if (active(ACT_FWD))    { move((cs * SPD) >> FIX, (sn * SPD) >> FIX); moving = true; }
            if (active(ACT_BACK))   { move(-((cs * SPD) >> FIX), -((sn * SPD) >> FIX)); moving = true; }
            if (active(ACT_SLEFT))  { move((sn * SPD) >> FIX, -((cs * SPD) >> FIX)); moving = true; }
            if (active(ACT_SRIGHT)) { move(-((sn * SPD) >> FIX), (cs * SPD) >> FIX); moving = true; }
            if (wantFire) { playerFire(); }
            if (wantUse)  { useDoor(); }
        }
        wantFire = false; wantUse = false;

        updateDoors();
        updateActors();
        updateProjectiles();
        if (deadTic == 0) { checkPickups(); }

        /* eye height eases onto whatever floor we are standing on */
        var target = floorz(px >> FIX, py >> FIX) + EYE_H;
        if (eyeZ < target)      { eyeZ += 10; if (eyeZ > target) { eyeZ = target; } }
        else if (eyeZ > target) { eyeZ -= 10; if (eyeZ < target) { eyeZ = target; } }

        /* weapon bob follows footsteps; settles when standing still */
        if (moving) {
            bobPhase = (bobPhase + 64) & NA_MASK;
            bobX = (cosTab[bobPhase] * 5) >> FIX;
            bobY = (cosTab[((bobPhase << 1) - NA_90) & NA_MASK] * 4) >> FIX;
            if (bobY < 0) { bobY = -bobY; }
        } else {
            if (bobX > 0) { bobX--; } else if (bobX < 0) { bobX++; }
            if (bobY > 0) { bobY--; }
        }

        if (wpnTic > 0) {
            wpnTic--;
            wpnFrame = (wpnTic > 7) ? 1 : ((wpnTic > 3) ? 2 : 0);
        } else {
            wpnFrame = 0;
        }

        curPal = (flashTic > 0) ? 2 : ((painTic > 0) ? 1 : 0);
        if (flashTic > 0) { flashTic--; }
        if (painTic > 0)  { painTic--; }
    }

    /* One micro-benchmark per frame, printed to the log.  On the watch the
     * log is GARMIN/Apps/LOGS/DoomCE.TXT (create the empty file first). */
    function bench(dc) {
        var t0 = System.getTimer(); var n = 0;
        var st = benchStep;
        if (st == 0) {
            System.println("bench: device " + System.getDeviceSettings().partNumber + " ciq " + Lang.format("$1$.$2$", [System.getDeviceSettings().monkeyVersion[0], System.getDeviceSettings().monkeyVersion[1]]));
        } else if (st == 1) {                                   /* interpreter: 5000 iterations */
            var arr = cosTab; var acc = 0;
            for (var i = 0; i < 5000; i++) { acc += arr[i & 15] + (i >> 3); }
            n = 5000;
        } else if (st == 2) {                                   /* 200 x fillRectangle 16x200 */
            dc.setColor(0x804020, Graphics.COLOR_TRANSPARENT);
            for (var i = 0; i < 200; i++) { dc.fillRectangle((i & 27) << 4, 50 + (i & 7), 16, 200); }
            n = 200;
        } else if (st == 3) {                                   /* 200 x fillRectangle 16x8 */
            dc.setColor(0x408020, Graphics.COLOR_TRANSPARENT);
            for (var i = 0; i < 200; i++) { dc.fillRectangle((i & 27) << 4, 50 + (i & 7), 16, 8); }
            n = 200;
        } else if (st == 4) {                                   /* 200 x setColor+fillRectangle */
            for (var i = 0; i < 200; i++) { dc.setColor(0x400000 + i, Graphics.COLOR_TRANSPARENT); dc.fillRectangle((i & 27) << 4, 50 + (i & 7), 16, 8); }
            n = 200;
        } else if (st == 5) {                                   /* make a 64x32 palette bitmap */
            var pl = [0x000000, 0x803020, 0xA04030, 0xC05040, 0x603020, 0x402010, 0xE0C080, 0x202020];
            benchBmp = Graphics.createBufferedBitmap({ :width => 64, :height => 32, :palette => pl }).get();
            var bd = benchBmp.getDc();
            for (var y = 0; y < 32; y++) { bd.setColor(pl[(y >> 2) & 7], pl[0]); bd.fillRectangle(0, y, 64, 1); }
            n = 1;
        } else if (st == 6) {                                   /* 200 x drawBitmap 64x32 */
            for (var i = 0; i < 200; i++) { dc.drawBitmap((i & 27) << 4, 50 + (i & 7), benchBmp); }
            n = 200;
        } else if (st == 7) {                                   /* 200 x drawScaledBitmap -> 16x200 */
            for (var i = 0; i < 200; i++) { dc.drawScaledBitmap((i & 27) << 4, 50 + (i & 7), 16, 200, benchBmp); }
            n = 200;
        } else if (st == 8) {                                   /* 50 x clipped drawScaledBitmap 1024x200 -> 16 wide */
            for (var i = 0; i < 50; i++) {
                var x = (i & 27) << 4;
                dc.setClip(x, 50, 16, 200);
                dc.drawScaledBitmap(x - ((i & 63) << 4), 50, 1024, 200, benchBmp);
            }
            dc.clearClip();
            n = 50;
        } else if (st == 9) {                                   /* 200 x setClip */
            for (var i = 0; i < 200; i++) { dc.setClip((i & 27) << 4, 50, 16, 200); }
            dc.clearClip();
            n = 200;
        } else if (st == 10) {                                  /* 2000 x ByteArray read+lut */
            var lut = shadeRGB[0]; var tex = wallTex; var acc = 0;
            for (var i = 0; i < 2000; i++) { acc += lut[tex[i & 2047]]; }
            n = 2000;
        } else if (st == 11) {                                  /* 500 x method call */
            var acc = 0;
            for (var i = 0; i < 500; i++) { acc += floorz(i & 15, i & 7); }
            n = 500;
        } else {
            System.println("bench: done");
            benchStep = 999; return;
        }
        System.println("bench " + st + ": n=" + n + " ms=" + (System.getTimer() - t0));
        benchStep = st + 1;
    }

    function render(dc) {
        if (!initDone()) { renderLoading(dc); return; }
        if (BENCH && benchStep < 999) { bench(dc); return; }

        mCalls = 0; mLastCol = -1; mIters = 0;
        var t0 = System.getTimer();
        renderWalls(dc);
        var t1 = System.getTimer();
        var wallIters = mIters;
        renderThings(dc);
        var t2 = System.getTimer();
        if (deadTic == 0) {
            renderCrosshair(dc);
            renderWeapon(dc);
        }
        /* pain / muzzle flash: one translucent fill over the view */
        if (curPal != 0 && tintPain != null) {
            dc.setColor(curPal == 1 ? tintPain : tintFlash, Graphics.COLOR_TRANSPARENT);
            dc.fillRectangle(0, 0, SCR_W, VIEW_H);
            mLastCol = -1;
        }
        var t3 = System.getTimer();
        renderHud(dc);
        var t4 = System.getTimer();
        frameCalls = mCalls;
        tWalls += t1 - t0; tThings += t2 - t1; tWeapon += t3 - t2; tHud += t4 - t3;

        var t = System.getTimer();
        if (lastT != 0) {
            fpsAcc += t - lastT; fpsN++;
            if (fpsN >= 8) {
                curFps = fpsAcc > 0 ? (8000 / fpsAcc) : 0;
                fpsAcc = 0; fpsN = 0;
                if (PROFILE) {
                    System.println("fps=" + curFps + " calls=" + frameCalls + " wallIters=" + wallIters + " spriteIters=" + (mIters - wallIters)
                                   + " ms/8f walls=" + tWalls + " things=" + tThings + " weapon=" + tWeapon + " hud=" + tHud);
                }
                tWalls = 0; tThings = 0; tWeapon = 0; tHud = 0;
            }
        }
        lastT = t;
    }
}
