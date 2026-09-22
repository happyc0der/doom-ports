#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

uint8_t  host_buf[2][320*240];
uint16_t host_pal[256];
uint8_t *host_draw = host_buf[0];
uint8_t  host_color, host_fg;
int      host_tx, host_ty;
uint8_t  kb_Data[8];
uint32_t timer_Control, timer_1_Counter;

#define BUF_A host_buf[0]
#define BUF_B host_buf[1]
#define main  ce_main
#include "../src/main.c"
#undef main

void kb_Scan(void) {}
void gfx_SwapDraw(void) { host_draw = (host_draw==host_buf[0])?host_buf[1]:host_buf[0]; }

/* --- 24-bit overflow tripwire: re-run the hot expressions and check ---- */
static long worst = 0;
static void chk(long v){ long a = v<0?-v:v; if(a>worst) worst=a; }

static void dump(const char *path, const uint8_t *fb)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n320 240\n255\n");
    for (int i = 0; i < 320*240; i++) {
        uint16_t c = host_pal[fb[i]];
        unsigned char rgb[3] = { (unsigned char)(( c        & 31) * 255 / 31),
                                 (unsigned char)(((c >> 5)  & 31) * 255 / 31),
                                 (unsigned char)(((c >> 10) & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    init_world();

    /* placement guard: nothing may start life inside a wall */
    {
        int fail = 0;
        if (solid_at(px, py)) {
            printf("FAIL: spawn (%d.%02d, %d.%02d) is inside a wall\n",
                   px>>FIX, ((px&255)*100)>>FIX, py>>FIX, ((py&255)*100)>>FIX);
            fail = 1;
        }
        for (int i = 0; i < nact; i++)
            if (solid_at(act[i].x, act[i].y)) {
                printf("FAIL: actor %d at cell (%d,%d) is inside a wall\n",
                       i, act[i].x>>FIX, act[i].y>>FIX);
                fail = 1;
            }
        if (fail) return 1;
        printf("placement ok: spawn + %d actors all in open cells\n", nact);
    }

    gfx_SetDrawBuffer();
    back = host_buf[1];
    init_palette(); init_tables(); init_textures(); init_actor_gfx(); init_font();

    if (argc > 4) {
        px = (int)(atof(argv[2]) * ONE);
        py = (int)(atof(argv[3]) * ONE);
        pa = atoi(argv[4]) & NA_MASK;
    }

    render_walls();
    render_things();
    render_crosshair();
    render_weapon();
    render_hud();
    dump(argc > 1 ? argv[1] : "frame.ppm", back);

    /* sweep every angle from a lot of positions; watch for 24-bit overflow
     * and for any obviously broken column */
    for (int my = 1; my < MAPH - 1; my++)
    for (int mx = 1; mx < MAPW - 1; mx++) {
        if (world[my][mx]) continue;
        px = mx * ONE + ONE/2; py = my * ONE + ONE/2;
        for (int a = 0; a < NA; a += 37) {
            pa = a;
            render_walls();
            render_things();
            for (int x = 0; x < SCR_W; x++) {
                chk((long)zbuf[x]);
                if (zbuf[x] < DIST_MIN || zbuf[x] > DIST_MAX) {
                    printf("BAD zbuf %d at cell %d,%d ang %d col %d\n",
                           zbuf[x], mx, my, a, x);
                    return 1;
                }
            }
        }
    }
    printf("sweep clean; max |zbuf| = %ld\n", worst);
    return 0;
}
