/* minimal host stubs so the renderer can be compiled and eyeballed on a PC */
#ifndef HOST_GRAPHX_H
#define HOST_GRAPHX_H
#include <stdint.h>
#include <string.h>
extern uint8_t  host_buf[2][320*240];
extern uint16_t host_pal[256];
extern uint8_t *host_draw;
#define gfx_palette host_pal
#define gfx_RGBTo1555(r,g,b) ((uint16_t)((((b)>>3)<<10)|(((g)>>3)<<5)|((r)>>3)))
static inline void gfx_Begin(void){}
static inline void gfx_End(void){}
static inline void gfx_SetDrawBuffer(void){ host_draw = host_buf[1]; }
void gfx_SwapDraw(void);   /* provided by the host program */
extern uint8_t host_color, host_fg; extern int host_tx, host_ty;
static inline void gfx_SetColor(uint8_t c){ host_color=c; }
static inline void gfx_SetTextFGColor(uint8_t c){ host_fg=c; }
static inline void gfx_SetTextXY(int x,int y){ host_tx=x; host_ty=y; }
static inline void gfx_PrintString(const char*s){ (void)s; }
static inline void gfx_PrintInt(int n,uint8_t l){ (void)n;(void)l; }
static inline void gfx_PrintUInt(unsigned n,uint8_t l){ (void)n;(void)l; }
static inline void gfx_FillRectangle(int x,int y,int w,int h){
    for(int j=y;j<y+h;j++) if(j>=0&&j<240) for(int i=x;i<x+w;i++) if(i>=0&&i<320) host_draw[j*320+i]=host_color; }
static inline void gfx_HorizLine(int x,int y,int w){ gfx_FillRectangle(x,y,w,1); }
#endif
