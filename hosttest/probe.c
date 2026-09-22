#include <stdint.h>
#include <stdio.h>
#include <string.h>
uint8_t host_buf[2][320*240]; uint16_t host_pal[256];
uint8_t *host_draw=host_buf[0]; uint8_t host_color,host_fg; int host_tx,host_ty;
uint8_t kb_Data[8]; uint32_t timer_Control,timer_1_Counter;
#define BUF_A host_buf[0]
#define BUF_B host_buf[1]
#define main ce_main
#include "../src/main.c"
#undef main
void kb_Scan(void){} void gfx_SwapDraw(void){}
int main(void){
    init_world(); init_palette(); init_tables(); init_textures(); init_actor_gfx();
    printf("shade ramps at each level (uniform base C(3,21) = %d):\n", C(3,21));
    for(int l=0;l<8;l++)
        printf("  lv%d: uniform->bright %2d   outline C(0,2)->bright %2d\n",
               l, shade[l][C(3,21)]&31, shade[l][C(0,2)]&31);
    printf("\ndistance -> shade level:\n");
    for(int cells=1;cells<=10;cells++){
        int fwd=cells*ONE, lv=fwd>>9; if(lv>7)lv=7;
        printf("  %2d cells (fwd %5d) -> lv %d  uniform bright %2d\n",
               cells, fwd, lv, shade[lv][C(3,21)]&31);
    }
    return 0;
}
