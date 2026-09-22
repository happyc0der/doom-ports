/* Relative cost of each stage, same source as the calculator build. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
                         return t.tv_sec*1e3 + t.tv_nsec/1e6; }

#define N 400
int main(void)
{
    double tw=0, tt=0, tg=0, th=0, tu=0, t0;
    init_world(); host_draw=host_buf[1]; back=host_buf[1];
    init_palette(); init_tables(); init_textures(); init_actor_gfx(); init_font();

    /* wake the actors so AI + projectiles are actually exercised */
    for (int i=0;i<nact;i++) act[i].st = A_CHASE;

    for (int f=0; f<N; f++) {
        int cs=cosTab[pa], sn=SIN(pa);
        if (f%3==0) pa=(pa+9)&NA_MASK;
        move((cs*6)>>FIX,(sn*6)>>FIX);

        t0=now(); update_doors(); update_actors(); update_projectiles();
                  check_pickups();                       tu += now()-t0;
        t0=now(); render_walls();                        tw += now()-t0;
        t0=now(); render_things();                       tt += now()-t0;
        t0=now(); render_crosshair(); render_weapon();   tg += now()-t0;
        t0=now(); render_hud();                          th += now()-t0;
    }
    double tot=tw+tt+tg+th+tu;
    printf("  %-22s %8.3f ms/frame  %5.1f%%\n","walls",       tw/N, 100*tw/tot);
    printf("  %-22s %8.3f ms/frame  %5.1f%%\n","things",      tt/N, 100*tt/tot);
    printf("  %-22s %8.3f ms/frame  %5.1f%%\n","weapon+cross",tg/N, 100*tg/tot);
    printf("  %-22s %8.3f ms/frame  %5.1f%%\n","hud",         th/N, 100*th/tot);
    printf("  %-22s %8.3f ms/frame  %5.1f%%\n","logic/AI",    tu/N, 100*tu/tot);
    printf("  %-22s %8.3f ms/frame\n","TOTAL", tot/N);
    return 0;
}
