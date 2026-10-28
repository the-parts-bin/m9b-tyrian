// The SDL stand-in (SDL.h) for OpenTyrian on the m9b SoC: start-up
// (traps and interrupts), time, input events, 8 bpp surfaces and the sound
// converter. Video is in video_m9b.c, audio output in loudness_m9b.c.
//
// Input: USB keyboards and mice on either of the core's two USB ports
// (../../core/rtl/usb_port.v, polled), and keys from the UART (run.py's
// terminal, tools/tyr.py): arrows, Enter, Esc, other printable characters.
// A terminal only sends presses, so a UART key counts as held until 150 ms
// after its last repeat. The UART also takes tools/tyr.py's screenshot
// request (0x1C: dump the screen, see video_m9b.c).
#include "SDL.h"
#include "m9b_map.h"

#include <stdlib.h>

#include "clock.h"   // CLOCK_HZ: ../../core/firmware

#define CLOCK_CNT     (*(volatile uint32_t*)(M9B_TIMING + 0xCu))
#define UART_DATA     (*(volatile uint32_t*)M9B_UART)
#define UART_RX_VALID (1u << 8)
#define IRQ_PENDING   (*(volatile uint32_t*)(M9B_CTRL + 0x4u))
#define IRQ_ENABLE    (*(volatile uint32_t*)(M9B_CTRL + 0x8u))

int SDL_PushEvent(SDL_Event *ev);
extern void trap_entry(void);        // ../../core/runtime/trap_entry.S
extern void m9b_music_poll(void);    // loudness_m9b.c: keeps the music stream topped up
extern void m9b_screenshot(void);    // video_m9b.c

// -------------------------------------------------------------------------
// Start-up, errors, logging
// -------------------------------------------------------------------------
int SDL_Init(Uint32 flags) {
    (void)flags;
    static int done;
    if (done) return 0;
    done = 1;
    asm volatile("csrw mtvec, %0" :: "r"(&trap_entry));
    IRQ_PENDING = 0xFu;
    IRQ_ENABLE = 1u;   // vertical blank (the page flip); audio added by loudness_m9b.c
    asm volatile("csrs mie, %0" :: "r"(1u << 11));
    asm volatile("csrs mstatus, %0" :: "r"(1u << 3));
    // The "window" has the focus (keyboard.c pauses the game without it).
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.window.type = SDL_WINDOWEVENT;
    ev.window.event = SDL_WINDOWEVENT_FOCUS_GAINED;
    SDL_PushEvent(&ev);
    return 0;
}
int SDL_InitSubSystem(Uint32 flags) { return SDL_Init(flags); }
void SDL_QuitSubSystem(Uint32 flags) { (void)flags; }
void SDL_Quit(void) {}
const char *SDL_GetError(void) { return "m9b"; }

size_t SDL_strlcpy(char *dst, const char *src, size_t size) {
    size_t n = strlen(src);
    if (size) {
        size_t c = n < size - 1 ? n : size - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}

char *SDL_GetBasePath(void) { return NULL; }
int SDL_SetHint(const char *name, const char *value) { (void)name; (void)value; return 1; }

void SDL_LogSetPriority(int category, int priority) { (void)category; (void)priority; }
void SDL_LogMessageV(int category, int priority, const char *fmt, va_list ap) {
    (void)category; (void)priority;
    vprintf(fmt, ap);
    putchar('\n');
}
#define LOG_BODY(pri) { va_list ap; va_start(ap, fmt); SDL_LogMessageV(0, pri, fmt, ap); va_end(ap); }
void SDL_Log(const char *fmt, ...) LOG_BODY(3)
void SDL_LogDebug(int category, const char *fmt, ...) { (void)category; LOG_BODY(2) }
void SDL_LogWarn(int category, const char *fmt, ...) { (void)category; LOG_BODY(4) }
void SDL_LogError(int category, const char *fmt, ...) { (void)category; LOG_BODY(5) }
int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, void *window) {
    (void)flags; (void)window;
    printf("%s: %s\n", title, message);
    return 0;
}

// -------------------------------------------------------------------------
// Time: milliseconds from the 36 MHz CLOCK_CNT, extended past its wrap.
// -------------------------------------------------------------------------
Uint32 SDL_GetTicks(void) {
    static uint32_t last, ms, frac;
    uint32_t now = CLOCK_CNT;
    frac += now - last;
    last = now;
    ms += frac / CLOCK_PER_MS;
    frac %= CLOCK_PER_MS;
    return ms;
}

uint32_t m9b_idle_cycles;   // time spent in SDL_Delay (the profile report)

void SDL_Delay(Uint32 ms) {
    uint32_t t0 = CLOCK_CNT;
    m9b_music_poll();
    while (CLOCK_CNT - t0 < ms * CLOCK_PER_MS) {
    }
    m9b_idle_cycles += CLOCK_CNT - t0;
}

// -------------------------------------------------------------------------
// Surfaces
// -------------------------------------------------------------------------
static SDL_PixelFormat format8 = {0, 8, 1};

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int depth, Uint32 rm, Uint32 gm, Uint32 bm, Uint32 am) {
    (void)flags; (void)rm; (void)gm; (void)bm; (void)am;
    if (depth != 8) return NULL;
    SDL_Surface *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->format = &format8;
    s->w = w;
    s->h = h;
    s->pitch = w;
    s->pixels = aligned_alloc(64, ((size_t)w * h + 63u) & ~63u);   // whole data L1 lines
    if (!s->pixels) { free(s); return NULL; }
    memset(s->pixels, 0, (size_t)w * h);
    return s;
}

void SDL_FreeSurface(SDL_Surface *s) {
    if (!s) return;
    free(s->pixels);
    free(s);
}

int SDL_FillRect(SDL_Surface *s, const SDL_Rect *r, Uint32 color) {
    SDL_Rect all = {0, 0, s->w, s->h}, c = r ? *r : all;
    if (c.x < 0) { c.w += c.x; c.x = 0; }
    if (c.y < 0) { c.h += c.y; c.y = 0; }
    if (c.x + c.w > s->w) c.w = s->w - c.x;
    if (c.y + c.h > s->h) c.h = s->h - c.y;
    if (c.w <= 0 || c.h <= 0) return 0;
    Uint8 *p = (Uint8 *)s->pixels + c.y * s->pitch + c.x;
    if (c.x == 0 && c.w == s->pitch) { memset(p, (int)color, (size_t)c.w * c.h); return 0; }
    for (int y = 0; y < c.h; y++, p += s->pitch) memset(p, (int)color, (size_t)c.w);
    return 0;
}

int SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *sr, SDL_Surface *dst, SDL_Rect *dr) {
    SDL_Rect s = sr ? *sr : (SDL_Rect){0, 0, src->w, src->h};
    int dx = dr ? dr->x : 0, dy = dr ? dr->y : 0;
    if (dx < 0) { s.x -= dx; s.w += dx; dx = 0; }
    if (dy < 0) { s.y -= dy; s.h += dy; dy = 0; }
    if (dx + s.w > dst->w) s.w = dst->w - dx;
    if (dy + s.h > dst->h) s.h = dst->h - dy;
    for (int y = 0; y < s.h; y++)
        memcpy((Uint8 *)dst->pixels + (dy + y) * dst->pitch + dx,
               (const Uint8 *)src->pixels + (s.y + y) * src->pitch + s.x, (size_t)s.w);
    return 0;
}

// The one texture format video_m9b.c announces: RGB565, the video engine's
// palette entries.
SDL_PixelFormat *SDL_AllocFormat(Uint32 format) {
    SDL_PixelFormat *f = calloc(1, sizeof(*f));
    f->format = format;
    f->BitsPerPixel = 16;
    f->BytesPerPixel = 2;
    return f;
}
void SDL_FreeFormat(SDL_PixelFormat *f) { free(f); }
const char *SDL_GetPixelFormatName(Uint32 format) { (void)format; return "RGB565"; }
Uint32 SDL_MapRGB(const SDL_PixelFormat *f, Uint8 r, Uint8 g, Uint8 b) {
    (void)f;
    return (Uint32)(r >> 3) << 11 | (Uint32)(g >> 2) << 5 | (b >> 3);
}

int SDL_GetNumVideoDisplays(void) { return 1; }
int SDL_GetWindowDisplayIndex(SDL_Window *w) { (void)w; return 0; }
void SDL_GetWindowSize(SDL_Window *w, int *width, int *height) {
    (void)w;
    if (width) *width = 320;
    if (height) *height = 200;
}
int SDL_ShowCursor(int toggle) { (void)toggle; return 0; }

// -------------------------------------------------------------------------
// Scancode names (the config file stores keys by name, as SDL2 does).
// -------------------------------------------------------------------------
static const char *const sc_names[] = {
    [SDL_SCANCODE_RETURN] = "Return", [SDL_SCANCODE_ESCAPE] = "Escape",
    [SDL_SCANCODE_BACKSPACE] = "Backspace", [SDL_SCANCODE_TAB] = "Tab",
    [SDL_SCANCODE_SPACE] = "Space", [SDL_SCANCODE_MINUS] = "-", [SDL_SCANCODE_EQUALS] = "=",
    [SDL_SCANCODE_LEFTBRACKET] = "[", [SDL_SCANCODE_RIGHTBRACKET] = "]",
    [SDL_SCANCODE_BACKSLASH] = "\\", [SDL_SCANCODE_NONUSHASH] = "#",
    [SDL_SCANCODE_SEMICOLON] = ";", [SDL_SCANCODE_APOSTROPHE] = "'", [SDL_SCANCODE_GRAVE] = "`",
    [SDL_SCANCODE_COMMA] = ",", [SDL_SCANCODE_PERIOD] = ".", [SDL_SCANCODE_SLASH] = "/",
    [SDL_SCANCODE_CAPSLOCK] = "CapsLock",
    [SDL_SCANCODE_F1] = "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    [SDL_SCANCODE_PRINTSCREEN] = "PrintScreen", "ScrollLock", "Pause", "Insert", "Home",
    "PageUp", "Delete", "End", "PageDown", "Right", "Left", "Down", "Up", "Numlock",
    [SDL_SCANCODE_KP_DIVIDE] = "Keypad /", "Keypad *", "Keypad -", "Keypad +", "Keypad Enter",
    "Keypad 1", "Keypad 2", "Keypad 3", "Keypad 4", "Keypad 5", "Keypad 6", "Keypad 7",
    "Keypad 8", "Keypad 9", "Keypad 0", "Keypad .",
    [SDL_SCANCODE_LCTRL] = "Left Ctrl", "Left Shift", "Left Alt", "Left GUI",
    "Right Ctrl", "Right Shift", "Right Alt", "Right GUI",
};

const char *SDL_GetScancodeName(SDL_Scancode sc) {
    static char one[2];
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) { one[0] = (char)('A' + sc - SDL_SCANCODE_A); return one; }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) { one[0] = (char)('1' + sc - SDL_SCANCODE_1); return one; }
    if (sc == SDL_SCANCODE_0) return "0";
    if ((unsigned)sc < sizeof(sc_names) / sizeof(*sc_names) && sc_names[sc]) return sc_names[sc];
    return "";
}

SDL_Scancode SDL_GetScancodeFromName(const char *name) {
    for (int sc = 1; sc < 256; sc++) {
        const char *n = SDL_GetScancodeName((SDL_Scancode)sc);
        if (*n && !strcasecmp(n, name)) return (SDL_Scancode)sc;
    }
    return SDL_SCANCODE_UNKNOWN;
}

// Keycodes: characters for the keys that type one (unshifted), else
// the scancode with SDLK_SCANCODE_MASK, as in SDL2.
static const char sc_chars[0x39] = {
    [SDL_SCANCODE_A] = 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '\r', 27, 8, '\t', ' ', '-', '=', '[', ']', '\\', '#', ';', '\'', '`', ',', '.', '/',
};
static const char sc_shifted[0x39] = {
    [SDL_SCANCODE_1] = '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    [SDL_SCANCODE_MINUS] = '_', '+', '{', '}', '|', '~', ':', '"', '~', '<', '>', '?',
};

static SDL_Keycode keycode_of(SDL_Scancode sc) {
    if ((unsigned)sc < sizeof(sc_chars) && sc_chars[sc]) return sc_chars[sc];
    return (SDL_Keycode)sc | SDLK_SCANCODE_MASK;
}

static int text_input;
void SDL_StartTextInput(void) { text_input = 1; }
void SDL_StopTextInput(void) { text_input = 0; }

// -------------------------------------------------------------------------
// Event queue
// -------------------------------------------------------------------------
#define QLEN 64u
static SDL_Event queue[QLEN];
static uint32_t q_head, q_tail;

int SDL_PushEvent(SDL_Event *ev) {
    if (q_tail - q_head >= QLEN) return 0;
    queue[q_tail++ % QLEN] = *ev;
    return 1;
}

static Uint16 mods;   // current modifier state (KMOD_*)

static void key_event(int down, SDL_Scancode sc) {
    static const Uint16 mod_bits[8] = {KMOD_LCTRL, KMOD_LSHIFT, KMOD_LALT, KMOD_LGUI,
                                       KMOD_RCTRL, KMOD_RSHIFT, KMOD_RALT, KMOD_RGUI};
    if (sc >= SDL_SCANCODE_LCTRL && sc <= SDL_SCANCODE_RGUI) {
        if (down) mods |= mod_bits[sc - SDL_SCANCODE_LCTRL];
        else mods &= (Uint16)~mod_bits[sc - SDL_SCANCODE_LCTRL];
    }
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.key.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    ev.key.keysym.scancode = sc;
    ev.key.keysym.sym = keycode_of(sc);
    ev.key.keysym.mod = mods;
    SDL_PushEvent(&ev);
    // Typed characters, as SDL_TEXTINPUT, while text input is on.
    if (down && text_input && (unsigned)sc < sizeof(sc_chars) && sc_chars[sc] >= ' ') {
        char c = sc_chars[sc];
        if (mods & KMOD_SHIFT) {
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            else if (sc_shifted[sc]) c = sc_shifted[sc];
        }
        memset(&ev, 0, sizeof(ev));
        ev.text.type = SDL_TEXTINPUT;
        ev.text.text[0] = c;
        SDL_PushEvent(&ev);
    }
}

// -------------------------------------------------------------------------
// UART keys
// -------------------------------------------------------------------------
#define HOLD_MS 150u
static struct { SDL_Scancode sc; uint32_t until; } held[8];
static uint32_t esc_state, esc_time;   // 0, 1 after ESC, 2 after ESC [

static void uart_key(SDL_Scancode sc, int shift) {
    uint32_t now = SDL_GetTicks();
    for (int i = 0; i < 8; i++)
        if (held[i].sc == sc) { held[i].until = now + HOLD_MS; return; }
    for (int i = 0; i < 8; i++)
        if (!held[i].sc) {
            held[i].sc = sc;
            held[i].until = now + HOLD_MS;
            if (shift) key_event(1, SDL_SCANCODE_LSHIFT);
            key_event(1, sc);
            if (shift) key_event(0, SDL_SCANCODE_LSHIFT);
            return;
        }
}

static SDL_Scancode scancode_of_char(char c, int *shift) {
    *shift = 0;
    if (c >= 'A' && c <= 'Z') { *shift = 1; c = (char)(c - 'A' + 'a'); }
    for (unsigned sc = SDL_SCANCODE_A; sc < sizeof(sc_chars); sc++)
        if (sc_chars[sc] == c) return (SDL_Scancode)sc;
    for (unsigned sc = SDL_SCANCODE_1; sc < sizeof(sc_shifted); sc++)
        if (sc_shifted[sc] == c) { *shift = 1; return (SDL_Scancode)sc; }
    return SDL_SCANCODE_UNKNOWN;
}

static void poll_uart(void) {
    uint32_t now = SDL_GetTicks();
    if (esc_state && now - esc_time > 30u) {   // a lone ESC
        esc_state = 0;
        uart_key(SDL_SCANCODE_ESCAPE, 0);
    }
    for (;;) {
        uint32_t d = UART_DATA;
        if (!(d & UART_RX_VALID)) break;
        unsigned char c = (unsigned char)d;
        if (esc_state == 1) {
            esc_state = c == '[' ? 2u : 0u;
            if (!esc_state) uart_key(SDL_SCANCODE_ESCAPE, 0);
            continue;
        }
        if (esc_state == 2) {
            esc_state = 0;
            if (c == 'A') uart_key(SDL_SCANCODE_UP, 0);
            else if (c == 'B') uart_key(SDL_SCANCODE_DOWN, 0);
            else if (c == 'C') uart_key(SDL_SCANCODE_RIGHT, 0);
            else if (c == 'D') uart_key(SDL_SCANCODE_LEFT, 0);
            continue;
        }
        if (c == 27) { esc_state = 1; esc_time = now; }
        else if (c == 0x1Cu) m9b_screenshot();
        else if (c == '\r' || c == '\n') uart_key(SDL_SCANCODE_RETURN, 0);
        else if (c == 8 || c == 127) uart_key(SDL_SCANCODE_BACKSPACE, 0);
        else if (c >= 32 && c < 127) {
            int shift;
            SDL_Scancode sc = scancode_of_char((char)c, &shift);
            if (sc) uart_key(sc, shift);
        }
    }
    for (int i = 0; i < 8; i++)
        if (held[i].sc && (int32_t)(now - held[i].until) >= 0) {
            key_event(0, held[i].sc);
            held[i].sc = SDL_SCANCODE_UNKNOWN;
        }
}

// -------------------------------------------------------------------------
// USB keyboards and mice (see ../../doom/port/doomgeneric_m9b.c)
// -------------------------------------------------------------------------
#define USB_REG(p, o)    (*(volatile uint32_t*)(M9B_USB + 0x20u * (p) + (o)))
#define USB_STATUS(p)    USB_REG(p, 0x00u)   // bits 1:0 type, 15:8 report count
#define USB_KEYS(p)      USB_REG(p, 0x04u)   // {k3, k2, k1, modifiers}
#define USB_KEYS2(p)     USB_REG(p, 0x08u)   // k4
#define USB_MOUSE(p)     USB_REG(p, 0x0Cu)   // {dy, dx, buttons}
#define USB_MOUSE_POS(p) USB_REG(p, 0x14u)   // {y, x}, 16-bit running sums

static struct {
    uint32_t type, count;
    uint8_t keys[4], mods;   // keyboard: the last report
    uint32_t pos;            // mouse: MOUSE_POS at the last poll
    uint8_t buttons;
} usb[2];

static int key_in(uint8_t k, const uint8_t *keys) {
    for (int i = 0; i < 4; i++)
        if (keys[i] == k) return 1;
    return 0;
}

static void usb_keys_changed(const uint8_t *old_keys, uint8_t old_mods, const uint8_t *keys, uint8_t new_mods) {
    for (int b = 0; b < 8; b++)
        if (((old_mods ^ new_mods) >> b) & 1u) key_event((new_mods >> b) & 1, (SDL_Scancode)(SDL_SCANCODE_LCTRL + b));
    for (int i = 0; i < 4; i++)
        if (old_keys[i] > 3u && !key_in(old_keys[i], keys)) key_event(0, (SDL_Scancode)old_keys[i]);
    for (int i = 0; i < 4; i++)
        if (keys[i] > 3u && !key_in(keys[i], old_keys)) key_event(1, (SDL_Scancode)keys[i]);
}

// The mouse: an absolute pointer over the 320 x 200 screen, from the
// hardware's running sums of every report.
static int32_t mouse_x = 160, mouse_y = 100;

static void usb_poll(void) {
    static const uint8_t none[4];
    int32_t dx = 0, dy = 0;
    uint8_t buttons = 0, old_buttons = 0;
    int mouse_seen = 0;
    for (uint32_t p = 0; p < 2u; p++) {
        uint32_t st = USB_STATUS(p), type = st & 3u, count = st >> 8 & 0xFFu;
        if (type != usb[p].type) {   // unplugged or replaced: release everything
            usb_keys_changed(usb[p].keys, usb[p].mods, none, 0);
            memset(usb[p].keys, 0, 4);
            usb[p].mods = 0;
            usb[p].buttons = 0;
            usb[p].pos = USB_MOUSE_POS(p);
            usb[p].type = type;
            usb[p].count = count;
            continue;
        }
        if (type == 2u) {
            uint32_t pos = USB_MOUSE_POS(p);
            dx += (int16_t)(pos - usb[p].pos);
            dy += (int16_t)((pos >> 16) - (usb[p].pos >> 16));
            usb[p].pos = pos;
            old_buttons |= usb[p].buttons;
            usb[p].buttons = (uint8_t)(USB_MOUSE(p) & 7u);
            buttons |= usb[p].buttons;
            mouse_seen = 1;
            continue;
        }
        if (type != 1u || count == usb[p].count) continue;
        usb[p].count = count;
        uint32_t k = USB_KEYS(p);
        uint8_t keys[4] = {(uint8_t)(k >> 8), (uint8_t)(k >> 16), (uint8_t)(k >> 24), (uint8_t)USB_KEYS2(p)};
        if (keys[0] == 0x01u) continue;   // ErrorRollOver
        usb_keys_changed(usb[p].keys, usb[p].mods, keys, (uint8_t)k);
        memcpy(usb[p].keys, keys, 4);
        usb[p].mods = (uint8_t)k;
    }
    if (!mouse_seen) return;
    SDL_Event ev;
    if (dx || dy) {
        mouse_x += dx;
        mouse_y += dy;
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_x > 319) mouse_x = 319;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_y > 199) mouse_y = 199;
        memset(&ev, 0, sizeof(ev));
        ev.motion.type = SDL_MOUSEMOTION;
        ev.motion.x = mouse_x;
        ev.motion.y = mouse_y;
        ev.motion.xrel = dx;
        ev.motion.yrel = dy;
        SDL_PushEvent(&ev);
    }
    // HID bits: 0 left, 1 right, 2 middle; SDL: 1 left, 2 middle, 3 right.
    static const Uint8 sdl_button[3] = {SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE};
    for (int b = 0; b < 3; b++)
        if (((buttons ^ old_buttons) >> b) & 1u) {
            memset(&ev, 0, sizeof(ev));
            int down = (buttons >> b) & 1;
            ev.button.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
            ev.button.button = sdl_button[b];
            ev.button.state = down ? SDL_PRESSED : SDL_RELEASED;
            ev.button.x = mouse_x;
            ev.button.y = mouse_y;
            SDL_PushEvent(&ev);
        }
}

static int relative_mouse;
int SDL_SetRelativeMouseMode(SDL_bool enabled) { relative_mouse = enabled; return 0; }

int SDL_PollEvent(SDL_Event *ev) {
    if (q_head == q_tail) {
        m9b_music_poll();
        poll_uart();
        usb_poll();
    }
    if (q_head == q_tail) return 0;
    if (ev) *ev = queue[q_head % QLEN];
    q_head++;
    return 1;
}

// -------------------------------------------------------------------------
// Sound conversion (nortsong.c: the effects, 8-bit signed 11025 Hz, to the
// mixer's format). loudness_m9b.c mixes 16-bit samples at their own rate
// (audioSampleRate = 11025), so this only widens them.
// -------------------------------------------------------------------------
int SDL_BuildAudioCVT(SDL_AudioCVT *cvt, SDL_AudioFormat sf, Uint8 sc, int sr, SDL_AudioFormat df, Uint8 dc, int dr) {
    memset(cvt, 0, sizeof(*cvt));
    if (sf != AUDIO_S8 || df != AUDIO_S16SYS || sc != 1 || dc != 1 || sr != dr) return -1;
    cvt->needed = 1;
    cvt->src_format = sf;
    cvt->dst_format = df;
    cvt->src_rate = sr;
    cvt->dst_rate = dr;
    cvt->len_mult = 2;
    cvt->len_ratio = 2.0;
    return 1;
}

int SDL_ConvertAudio(SDL_AudioCVT *cvt) {
    // In place, from the end: buf holds len bytes and has room for 2 * len.
    for (int i = cvt->len - 1; i >= 0; i--) {
        int16_t s = (int16_t)((int8_t)cvt->buf[i] * 256);
        memcpy(cvt->buf + 2 * i, &s, 2);
    }
    cvt->len_cvt = cvt->len * 2;
    return 0;
}

// time() (xmas.c checks the date): seconds since start-up, from 1970.
#include <sys/time.h>
int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    uint32_t ms = SDL_GetTicks();
    tv->tv_sec = ms / 1000u;
    tv->tv_usec = (ms % 1000u) * 1000u;
    return 0;
}
