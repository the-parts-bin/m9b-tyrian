// Video for OpenTyrian on the m9b SoC, in place of video.c and
// video_scale.c: the game draws into its 320 x 200 8 bpp surfaces
// (VGAScreen and friends) and JE_showVGA() copies VGAScreen into one of
// three display buffers, cleans it from the data cache and has the
// vertical-blank interrupt show it from the next frame, with the palette
// the game has set (rgb_palette[], RGB565 through SDL_MapRGB) loaded at the
// same time. The 200 lines are stretched to the 480 (2 or 3 each: DOS's
// 320 x 200 filled a 4:3 screen), columns doubled (40..679 through
// VIDEO_HWIN), as in the Doom port.
//
// m9b_screenshot() (a 0x1C byte on the UART, see m9b_sdl.c) dumps the
// screen on the UART for ../core/tools/m9bctl.py: "\nM9BSHOT\n", the width
// and height (16-bit little-endian), 256 RGB565 palette entries, then the
// pixels of the buffer on screen.
#include "video.h"

#include <stdlib.h>

#include "palette.h"
#include "video_scale.h"
#include "m9b_map.h"

#define VRAM          ((volatile uint32_t*)M9B_VRAM)
#define VRAM_PALETTE  (VRAM + 512)   // 256 x RGB565, two per word
#define VRAM_TABLE    (VRAM + 640)   // one entry per line
#define VIDEO_CTRL    (*(volatile uint32_t*)M9B_CTRL)
#define VIDEO_HWIN    (*(volatile uint32_t*)(M9B_CTRL + 0x18u))
#define UART_DATA     (*(volatile uint32_t*)M9B_UART)
#define UART_STATUS   (*(volatile uint32_t*)(M9B_UART + 0x4u))
#define PSRAM_BASE    M9B_PSRAM_BASE
#define LINE_BLANK    (0u << 22)
#define LINE_8BPP     (2u << 22)
#define LINE_2X       (1u << 28)

SDL_Surface *VGAScreen, *VGAScreenSeg;
SDL_Surface *VGAScreen2;
SDL_Surface *game_screen;
SDL_Window *main_window = NULL;
SDL_PixelFormat *main_window_tex_format = NULL;

int fullscreen_display = -1;
ScalingMode scaling_mode = SCALE_INTEGER;
const char *const scaling_mode_names[ScalingMode_MAX] = {"Center", "Integer", "Fit 8:5", "Fit 4:3"};

// One "scaler": the hardware's 2x.
uint scaler = 0;
const struct Scalers scalers[] = {{640, 400, NULL, NULL, "2x"}};
const uint scalers_count = 1;
void set_scaler_by_name(const char *name) { (void)name; }
bool init_scaler(unsigned int new_scaler) { (void)new_scaler; return true; }
bool set_scaling_mode_by_name(const char *name) { (void)name; return true; }
void video_on_win_resize(void) {}
void reinit_fullscreen(int new_display) { (void)new_display; }
void toggle_fullscreen(void) {}
void mapScreenPointToWindow(Sint32 *x, Sint32 *y) { (void)x; (void)y; }
void mapWindowPointToScreen(Sint32 *x, Sint32 *y) { (void)x; (void)y; }
void scaleWindowDistanceToScreen(Sint32 *x, Sint32 *y) { (void)x; (void)y; }

// Display buffers: one on screen, one waiting for the vertical blank, one
// to copy the next frame into.
#define NBUF 3
static uint8_t *disp[NBUF];
static volatile int shown = -1, pending = -1;
static uint32_t pal_words[128], pal_shown[128];
static volatile uint32_t pal_pending;

static uint32_t line_8bpp(uintptr_t addr) {
    uint32_t off = (uint32_t)addr - PSRAM_BASE;
    return LINE_8BPP | ((off & 255u) / 2u) << 15 | (off >> 8);
}

// From the trap handler (../../core/runtime/trap.c) at the vertical blank
// (line 480; the 43 blank lines leave about 1.3 ms before line 0 is
// fetched): points the 480 table entries at the new frame, each of its 200
// lines on 2 or 3 of them (row_of[]), and loads the palette.
static uint8_t row_of[480];

void vblank_flip(void) {
    int p = pending;
    if (p < 0) return;
    const uint8_t *buf = disp[p];
    if (!row_of[479])
        for (uint32_t y = 0; y < 480u; y++) row_of[y] = (uint8_t)(y * vga_height / 480u);
    for (uint32_t y = 0; y < 480u; y++)
        VRAM_TABLE[y] = line_8bpp((uintptr_t)&buf[row_of[y] * vga_width]) | LINE_2X;
    if (pal_pending) {
        for (uint32_t i = 0; i < 128u; i++) VRAM_PALETTE[i] = pal_words[i];
        pal_pending = 0;
    }
    shown = p;
    pending = -1;
}

void init_video(void) {
    VGAScreen = VGAScreenSeg = SDL_CreateRGBSurface(0, vga_width, vga_height, 8, 0, 0, 0, 0);
    VGAScreen2 = SDL_CreateRGBSurface(0, vga_width, vga_height, 8, 0, 0, 0, 0);
    game_screen = SDL_CreateRGBSurface(0, vga_width, vga_height, 8, 0, 0, 0, 0);
    main_window_tex_format = SDL_AllocFormat(SDL_PIXELFORMAT_RGB565);
    for (int i = 0; i < NBUF; i++) {
        disp[i] = aligned_alloc(256, vga_width * vga_height);
        memset(disp[i], 0, vga_width * vga_height);
    }
    for (uint32_t y = 0; y < 480u; y++) VRAM_TABLE[y] = LINE_BLANK;
    VIDEO_HWIN = (40u + 2u * vga_width) << 16 | 40u;
    VIDEO_CTRL = 1;
}

void deinit_video(void) {}

void JE_clr256(SDL_Surface *screen) {
    SDL_FillRect(screen, NULL, 0);
}

static void cache_clean(const void *p, uint32_t len) {
    for (uintptr_t a = (uintptr_t)p & ~63u; a < (uintptr_t)p + len; a += 64u)
        asm volatile("cbo.clean (%0)" :: "r"(a) : "memory");
}

static uint32_t irq_off(void) {
    uint32_t m;
    asm volatile("csrrci %0, mstatus, 8" : "=r"(m));
    return m & 8u;
}

static void irq_restore(uint32_t m) {
    if (m) asm volatile("csrsi mstatus, 8");
}

void JE_showVGA(void) {
    // A buffer that is neither on screen nor waiting to be.
    int b = 0;
    while (b == shown || b == pending) b++;
    memcpy(disp[b], VGAScreen->pixels, vga_width * vga_height);
    cache_clean(disp[b], vga_width * vga_height);
    // The palette, if it changed since the last frame handed over.
    uint32_t words[128];
    int changed = 0;
    for (uint32_t i = 0; i < 128u; i++) {
        words[i] = (rgb_palette[2 * i] & 0xFFFFu) | rgb_palette[2 * i + 1] << 16;
        changed |= words[i] != pal_shown[i];
    }
    uint32_t m = irq_off();
    if (changed) {
        memcpy(pal_words, words, sizeof(words));
        memcpy(pal_shown, words, sizeof(words));
        pal_pending = 1;
    }
    pending = b;   // replaces a frame still waiting, if any
    irq_restore(m);
#ifdef M9B_PROFILE
    // Every 5 s: frames shown, the time idle (SDL_Delay), and sound.
    extern uint32_t m9b_idle_cycles, sound_cycles;
    static uint32_t frames, t0, idle0, snd0;
    uint32_t now = *(volatile uint32_t *)(M9B_TIMING + 0xCu);
    if (frames++ == 0) { t0 = now; idle0 = m9b_idle_cycles; snd0 = sound_cycles; }
    else if (now - t0 >= 5u * 36000000u) {
        uint32_t dt = now - t0;
        extern volatile uint32_t mus_r;
        printf("%lu.%lu fps, idle %lu%%, sound %lu%%, music at %lu\n", (unsigned long)((frames - 1) * 36000000ull * 10 / dt / 10),
               (unsigned long)((frames - 1) * 36000000ull * 10 / dt % 10),
               (unsigned long)((uint64_t)(m9b_idle_cycles - idle0) * 100 / dt),
               (unsigned long)((uint64_t)(sound_cycles - snd0) * 100 / dt), (unsigned long)mus_r);
        frames = 0;
        static uint32_t reports;
        if (++reports % 3u == 0) {   // every 15 s: the PC samples (../../core/runtime/trap.c)
            extern void prof_report(void);
            prof_report();
        }
    }
#endif
}

static void uart_raw(uint8_t c) {
    while (UART_STATUS & 1u) {
    }
    UART_DATA = c;
}

void m9b_screenshot(void) {
    static const char tag[] = "\nM9BSHOT\n";
    for (const char *s = tag; *s; s++) uart_raw((uint8_t)*s);
    uart_raw(vga_width & 255u); uart_raw(vga_width >> 8);
    uart_raw(vga_height & 255u); uart_raw(vga_height >> 8);
    for (uint32_t i = 0; i < 256u; i++) {
        uint16_t c = (uint16_t)rgb_palette[i];
        uart_raw((uint8_t)c);
        uart_raw((uint8_t)(c >> 8));
    }
    // What is on screen (VGAScreen may be an off-screen buffer mid-frame).
    const uint8_t *px = shown >= 0 ? disp[shown] : VGAScreen->pixels;
    for (uint32_t i = 0; i < vga_width * vga_height; i++) uart_raw(px[i]);
}
