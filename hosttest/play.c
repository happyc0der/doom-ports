/* ------------------------------------------------------------------
 * Desktop driver for the DOOMCE engine.
 *
 * Compiles the *same* src/main.c that produces the .8xp.  graphx /
 * keypadc / tice are stubbed; SDL supplies input and presents the
 * palettized framebuffer.  ce_main() is the calculator's own game loop.
 *
 * The frame-rate cap exists so the feel matches the hardware: movement
 * in main.c is per-frame, so an uncapped 3000 fps makes the player
 * teleport.  Cap at what the CE actually manages and the tuning transfers.
 * ------------------------------------------------------------------ */

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

#define SCALE_DEFAULT 3

static SDL_Window   *win;
static SDL_Renderer *ren;
static SDL_Texture  *fbtex;
static uint32_t      rgba[320*240];
static int           g_running = 1;
static int           capFps = 20;
static int           showZ;          /* F1: visualise the depth buffer */
static Uint64        nextFrame, perfFreq;
static unsigned long totalFrames;
static const char   *exitWhy = "ce_main returned";
static unsigned      frames;
static Uint64        titleT0;
static double        measured;

/* ---- input ------------------------------------------------------- */

void kb_Scan(void)
{
    SDL_Event e;
    const Uint8 *k;
    int retune = 0;

    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { g_running = 0; exitWhy = "window closed"; }
        if (e.type != SDL_KEYDOWN) continue;
        switch (e.key.keysym.sym) {
        case SDLK_ESCAPE:     g_running = 0; exitWhy = "Esc pressed"; break;
        case SDLK_LEFTBRACKET:  if (moveSpeed > 1)  moveSpeed--; break;
        case SDLK_RIGHTBRACKET: if (moveSpeed < 80) moveSpeed++; break;
        case SDLK_MINUS:      if (turnSpeed > 1)  turnSpeed--; break;
        case SDLK_EQUALS:     if (turnSpeed < 200) turnSpeed++; break;
        case SDLK_COMMA:      if (projd > 120) { projd -= 8; retune = 1; } break;
        case SDLK_PERIOD:     if (projd < 600) { projd += 8; retune = 1; } break;
        case SDLK_1:          if (capFps > 5)  capFps -= 5; break;
        case SDLK_2:          if (capFps < 60) capFps += 5; break;
        case SDLK_F1:         showZ = !showZ; break;
        case SDLK_r:          health = 100; ammo = 50; kills = 0;
                              init_world(); break;
        default: break;
        }
    }
    if (retune) init_tables();          /* FOV change re-derives the ray tables */

    k = SDL_GetKeyboardState(NULL);
    memset(kb_Data, 0, sizeof kb_Data);

    if (k[SDL_SCANCODE_UP]    || k[SDL_SCANCODE_W]) kb_Data[7] |= kb_Up;
    if (k[SDL_SCANCODE_DOWN]  || k[SDL_SCANCODE_S]) kb_Data[7] |= kb_Down;
    if (k[SDL_SCANCODE_LEFT])                       kb_Data[7] |= kb_Left;
    if (k[SDL_SCANCODE_RIGHT])                      kb_Data[7] |= kb_Right;
    if (k[SDL_SCANCODE_A])                          kb_Data[1] |= kb_Yequ;   /* strafe L */
    if (k[SDL_SCANCODE_D])                          kb_Data[1] |= kb_Graph;  /* strafe R */
    if (k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_LCTRL]) kb_Data[1] |= kb_2nd;

    if (!g_running) kb_Data[6] |= kb_Clear;   /* how ce_main() exits */
}

/* ---- present ----------------------------------------------------- */

void gfx_SwapDraw(void)
{
    Uint64 now;

    if (showZ) {                         /* depth buffer over the viewport */
        for (int x = 0; x < SCR_W; x++) {
            int v = 31 - (zbuf[x] >> 8);
            uint8_t c = C(3, v < 0 ? 0 : v);
            for (int y = 0; y < 8; y++) host_draw[y*SCR_W + x] = c;
        }
    }

    for (int i = 0; i < 320*240; i++) {
        uint16_t c = host_pal[host_draw[i]];
        rgba[i] = 0xFF000000u
                | (uint32_t)(( c        & 31) * 255 / 31) << 16
                | (uint32_t)(((c >> 5)  & 31) * 255 / 31) << 8
                | (uint32_t)(((c >> 10) & 31) * 255 / 31);
    }
    SDL_UpdateTexture(fbtex, NULL, rgba, 320 * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, fbtex, NULL, NULL);
    SDL_RenderPresent(ren);

    host_draw = (host_draw == host_buf[0]) ? host_buf[1] : host_buf[0];
    totalFrames++;

    /* pace to capFps so per-frame movement feels like the hardware */
    now = SDL_GetPerformanceCounter();
    {
        Uint64 period = perfFreq / (Uint64)capFps;
        if (nextFrame == 0) nextFrame = now;
        nextFrame += period;
        if (nextFrame > now) {
            Uint64 waitMs = ((nextFrame - now) * 1000) / perfFreq;
            if (waitMs > 0) SDL_Delay((Uint32)waitMs);
        } else {
            nextFrame = now;            /* we're behind; don't spiral */
        }
    }

    timer_1_Counter = (uint32_t)((SDL_GetPerformanceCounter() * 32768) / perfFreq);

    if (++frames >= 10) {
        Uint64 t = SDL_GetPerformanceCounter();
        char title[200];
        measured = (double)frames * (double)perfFreq / (double)(t - titleT0);
        titleT0 = t; frames = 0;
        snprintf(title, sizeof title,
                 "DOOMCE  |  %.0f fps (cap %d)  |  speed %d  turn %d  projd %d"
                 "  |  HP %d  AMMO %d  KILLS %d",
                 measured, capFps, moveSpeed, turnSpeed, projd, health, ammo, kills);
        SDL_SetWindowTitle(win, title);
    }
}

/* ---- entry ------------------------------------------------------- */

int main(int argc, char **argv)
{
    int scale = SCALE_DEFAULT, headless = 0, nframes = 0;
    const char *shot = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i+1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i+1 < argc) capFps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i+1 < argc) { headless = 1; nframes = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--shot") && i+1 < argc) shot = argv[++i];
        else { fprintf(stderr,
                "usage: %s [--scale N] [--fps N] [--frames N --shot out.ppm]\n", argv[0]);
               return 1; }
    }
    if (scale < 1) scale = 1;
    if (capFps < 1) capFps = 1;

    SDL_SetMainReady();
    if (SDL_Init(headless ? 0 : SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    perfFreq = SDL_GetPerformanceFrequency();
    titleT0  = SDL_GetPerformanceCounter();

    if (headless) {
        /* smoke test: run the real loop, no window, dump the last frame */
        init_world();
        host_draw = host_buf[1];
        back = host_buf[1];
        init_palette(); init_tables(); init_textures(); init_actor_gfx(); init_font();
        for (int f = 0; f < nframes; f++) {
            memset(kb_Data, 0, sizeof kb_Data);
            kb_Data[7] |= kb_Up;                       /* walk forward */
            if (f % 3 == 0) kb_Data[7] |= kb_Right;    /* and turn */
            {
                const int SPD = moveSpeed, TURN = turnSpeed;
                int cs, sn;
                if (kb_Data[7] & kb_Right) pa = (pa + TURN) & NA_MASK;
                cs = cosTab[pa]; sn = SIN(pa);
                if (kb_Data[7] & kb_Up) move((cs*SPD)>>FIX, (sn*SPD)>>FIX);
            }
            render_walls(); render_things(); render_crosshair(); render_weapon(); render_hud();
        }
        if (shot) {
            FILE *fp = fopen(shot, "wb");
            fprintf(fp, "P6\n320 240\n255\n");
            for (int i = 0; i < 320*240; i++) {
                uint16_t c = host_pal[back[i]];
                unsigned char px[3] = {
                    (unsigned char)(( c        & 31) * 255 / 31),
                    (unsigned char)(((c >> 5)  & 31) * 255 / 31),
                    (unsigned char)(((c >> 10) & 31) * 255 / 31) };
                fwrite(px, 1, 3, fp);
            }
            fclose(fp);
        }
        printf("headless: %d frames ok, player at %d.%02d, %d.%02d ang %d\n",
               nframes, px>>FIX, ((px&255)*100)>>FIX,
                        py>>FIX, ((py&255)*100)>>FIX, pa);
        SDL_Quit();
        return 0;
    }

    win = SDL_CreateWindow("DOOMCE",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            320*scale, 240*scale, SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");        /* crisp pixels */
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(ren, 320, 240);
    fbtex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, 320, 240);
    if (!win || !ren || !fbtex) {
        fprintf(stderr, "SDL setup failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_RaiseWindow(win);
    SDL_ShowWindow(win);
    {
        SDL_RendererInfo ri;
        SDL_GetRendererInfo(ren, &ri);
        printf("window %dx%d (scale %d) | renderer %s | cap %d fps\n",
               320*scale, 240*scale, scale, ri.name ? ri.name : "?", capFps);
        printf("keys: arrows/WS move  AD strafe  space fire  [ ] speed  - = turn"
               "  , . fov  1 2 cap  F1 depth  R reset  Esc quit\n");
        fflush(stdout);
    }

    {
        Uint64 t0 = SDL_GetPerformanceCounter();
        double secs;

        ce_main();      /* the calculator's own main(), unmodified */

        secs = (double)(SDL_GetPerformanceCounter() - t0) / (double)perfFreq;
        printf("exit: %s after %lu frames in %.1f s (%.1f fps avg)\n",
               exitWhy, totalFrames, secs,
               secs > 0 ? (double)totalFrames / secs : 0.0);
    }

    SDL_DestroyTexture(fbtex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
