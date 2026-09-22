#ifndef HOST_KEYPADC_H
#define HOST_KEYPADC_H
#include <stdint.h>
extern uint8_t kb_Data[8];
void kb_Scan(void);        /* provided by the host program */
#define kb_Graph 0x01
#define kb_Yequ  0x10
#define kb_2nd   0x20
#define kb_Alpha 0x80
#define kb_Clear 0x40
#define kb_Down  0x01
#define kb_Left  0x02
#define kb_Right 0x04
#define kb_Up    0x08
#endif
