/* Dump every generated frame as one sheet, for eyeballing the art. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
void gfx_SwapDraw(void) {}

#define COLS 10
#define PAD  2

int main(void)
{
    int cw = SPR_W + PAD, ch = SPR_H + PAD;
    int W  = COLS * cw, H = 3 * ch + (WPN_H + PAD);
    static uint8_t sheet[400*400];
    FILE *f;

    init_world(); init_palette(); init_tables(); init_textures(); init_actor_gfx(); init_font();
    memset(sheet, C(4, 4), sizeof sheet);

    for (int i = 0; i < NFRAME; i++) {                 /* enemy frames */
        int ox = (i % COLS) * cw, oy = (i / COLS) * ch;
        for (int x = 0; x < SPR_W; x++)
            for (int y = 0; y < SPR_H; y++) {
                uint8_t c = eGfx[i][x*SPR_H + y];
                if (c) sheet[(oy+y)*W + ox+x] = c;
            }
    }
    for (int i = 0; i < NWPN; i++) {                   /* weapon frames */
        int ox = i * (WPN_W + PAD), oy = 2 * ch + PAD;
        for (int x = 0; x < WPN_W; x++)
            for (int y = 0; y < WPN_H; y++) {
                uint8_t c = wGfx[i][x*WPN_H + y];
                if (c) sheet[(oy+y)*W + ox+x] = c;
            }
    }

    f = fopen("sheet.ppm", "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W*H; i++) {
        uint16_t c = host_pal[sheet[i]];
        unsigned char px[3] = { (unsigned char)(( c        & 31)*255/31),
                                (unsigned char)(((c >> 5)  & 31)*255/31),
                                (unsigned char)(((c >> 10) & 31)*255/31) };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
    printf("sheet.ppm  %dx%d  (%d enemy frames, %d weapon frames)\n",
           W, H, NFRAME, NWPN);
    return 0;
}
