// Sound for OpenTyrian on the m9b SoC, in place of loudness.c (same API,
// loudness.h).
//
// The hardware plays 48 kHz 16-bit stereo from two 1 KB halves (256 frames)
// and raises an interrupt as it moves on from one; the interrupt (the trap
// handler -> sound_irq()) mixes the next half at 24 kHz, each result held
// for 2 frames, and hands it back (see ../../doom/port/sound_m9b.c, the
// same scheme).
//
// Effects: the game's 8 channels of 16-bit samples (nortsong.c converts
// its 8-bit 11025 Hz sounds, audioSampleRate = 11025), stepped through at
// 11025/24000 in 16.16 fixed point, with the game's 8 volume levels.
//
// Music: Tyrian's songs are Loudness (LDS) tunes for an OPL2, which the
// DOSBox emulator OpenTyrian uses can't do here (it works in floating
// point, per sample). tools/make_music.sh renders each song once through
// on a PC with the same player and emulator, to TYRIAN/MUSIC/SONGnn.WAV
// (24 kHz 16-bit mono), and this streams them from the SD card like the
// Doom port's music, looping, except for the songs the player stops at
// their end (an "m9b " chunk in the file says so): those end, and
// `playing` goes false, which is how the game knows to go back to the
// level's song. A song without a file is silent.
#include "loudness.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "SDL.h"
#include "clock.h"
#include "m9b_map.h"

#define IRQ_PENDING   (*(volatile uint32_t*)(M9B_CTRL + 0x4u))
#define IRQ_ENABLE    (*(volatile uint32_t*)(M9B_CTRL + 0x8u))
#define AUDIO_CTRL    (*(volatile uint32_t*)(M9B_CTRL + 0xCu))
#define AUDIO_PTR     ((volatile uint32_t*)(M9B_CTRL + 0x10u))   // [0] half A, [1] half B
#define PSRAM_BASE    M9B_PSRAM_BASE
#define CLOCK_CNT     (*(volatile uint32_t*)(M9B_TIMING + 0xCu))
#define IRQ_AUDIO_A   (1u << 1)
#define IRQ_AUDIO_B   (1u << 2)

#define FRAMES        256u      // per half
#define HOLD          2u        // output frames per mixed sample
#define MIX_RATE      24000u
#define SFX_RATE      11025u
#define CHANNEL_COUNT 8
#define CHANNEL_VOLUME_LEVELS 8

int audioSampleRate = SFX_RATE;
unsigned int song_playing = 0;
bool audio_disabled = false, music_disabled = false, samples_disabled = false;
bool playing, songlooped;   // lds_play.c's, for jukebox.c: the stream's state

uint32_t sound_cycles, sound_mixes;   // time in sound_irq (clock ticks), halves mixed

static uint32_t audio_buf[2][FRAMES] __attribute__((aligned(256)));
static uint32_t silence[FRAMES] __attribute__((aligned(256)));

static struct {
    const int16_t *data;   // NULL: idle
    uint32_t len, pos;     // pos 16.16
    int32_t vol;           // Q12
} channels[CHANNEL_COUNT];

static int32_t volume_table[256];   // Q12, 30 dB range (as loudness.c)
static int32_t music_vol = 1 << 12, sample_vol = 1 << 12;

static uint32_t irq_off(void) {
    uint32_t m;
    asm volatile("csrrci %0, mstatus, 8" : "=r"(m));
    return m & 8u;
}

static void irq_restore(uint32_t m) {
    if (m) asm volatile("csrsi mstatus, 8");
}

static void cache_clean(const void *p, uint32_t len) {
    for (uintptr_t a = (uintptr_t)p & ~63u; a < (uintptr_t)p + len; a += 64u)
        asm volatile("cbo.clean (%0)" :: "r"(a) : "memory");
}

// -------------------------------------------------------------------------
// Music stream: a 256 KB ring (5.5 s) filled from the main loop
// (m9b_music_poll, from SDL_PollEvent and SDL_Delay), read by the mixer.
// -------------------------------------------------------------------------
#define MUS_RATE      24000u
#define MUS_RING      (128u * 1024u)   // samples
#define MUS_CHUNK     (8u * 1024u)     // samples per read

static int16_t mus_ring[MUS_RING] __attribute__((aligned(64)));
volatile uint32_t mus_r;                  // (the profile report shows it)
static volatile uint32_t mus_w;
static volatile int mus_on;              // the mixer plays the ring
static int mus_fd = -1, mus_eof;
static int mus_once;                     // the song stops at its end instead of looping
static uint32_t mus_data_start, mus_data_end;
static int mus_missing;                  // song_playing has no file (don't look again)
static volatile int32_t mus_fade;        // Q12 fade factor; mus_fade_step > 0: fading out
static volatile int32_t mus_fade_step;

static void mus_close(void) {
    mus_on = 0;
    if (mus_fd >= 0) close(mus_fd);
    mus_fd = -1;
}

static int mus_open_wav(const char *path) {
    mus_fd = open(path, O_RDONLY);
    if (mus_fd < 0) return -1;
    uint8_t h[12];
    if (read(mus_fd, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return -1;
    uint32_t pos = 12;
    int fmt_ok = 0;
    mus_once = 0;
    for (;;) {
        uint8_t c[16];
        if (lseek(mus_fd, pos, SEEK_SET) != (off_t)pos || read(mus_fd, c, 8) != 8) return -1;
        uint32_t size = c[4] | c[5] << 8 | (uint32_t)c[6] << 16 | (uint32_t)c[7] << 24;
        if (!memcmp(c, "fmt ", 4)) {
            if (read(mus_fd, c, 16) != 16) return -1;
            uint32_t rate = c[4] | c[5] << 8 | (uint32_t)c[6] << 16 | (uint32_t)c[7] << 24;
            fmt_ok = c[0] == 1 && c[1] == 0 && c[2] == 1 && rate == MUS_RATE && c[14] == 16;
        } else if (!memcmp(c, "m9b ", 4)) {   // tools/musrender.c: byte 0 = 1, plays once
            if (read(mus_fd, c, 1) != 1) return -1;
            mus_once = c[0] & 1;
        } else if (!memcmp(c, "data", 4)) {
            if (!fmt_ok) return -1;
            mus_data_start = pos + 8u;
            mus_data_end = mus_data_start + (size & ~1u);
            return lseek(mus_fd, mus_data_start, SEEK_SET) == (off_t)mus_data_start ? 0 : -1;
        }
        pos += 8u + ((size + 1u) & ~1u);
    }
}

// Up to max_chunks reads of at most 16 KB, looping at the end.
static void mus_fill(int max_chunks) {
    if (mus_fd < 0 || mus_eof) return;
    while (max_chunks-- > 0) {
        uint32_t w = mus_w, space = MUS_RING - (w - mus_r);
        uint32_t at = w & (MUS_RING - 1u), n = MUS_RING - at;
        if (n > MUS_CHUNK) n = MUS_CHUNK;
        if (space < n) return;
        off_t pos = lseek(mus_fd, 0, SEEK_CUR);
        uint32_t left = (mus_data_end - (uint32_t)pos) / 2u;
        if (n > left) n = left;
        if (n) {
            ssize_t got = read(mus_fd, &mus_ring[at], n * 2u);
            if (got <= 0) { mus_eof = 1; return; }
            mus_w = w + (uint32_t)got / 2u;
            continue;
        }
        if (mus_once) { mus_eof = 1; return; }
        songlooped = true;
        if (lseek(mus_fd, mus_data_start, SEEK_SET) < 0) { mus_eof = 1; return; }
        max_chunks++;
    }
}

void m9b_music_poll(void) {
    if (mus_on) mus_fill(2);
    playing = mus_on && !(mus_eof && mus_r == mus_w);
}

// -------------------------------------------------------------------------
// The mixer (interrupt)
// -------------------------------------------------------------------------
static inline int32_t clamp16(int32_t v) {
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

static void mix_half(uint32_t half) {
    uint32_t *out = audio_buf[half];
    int any = 0;
    int32_t acc[FRAMES / HOLD];
    // Music first (writes acc), then the channels (add).
    if (mus_on) {
        uint32_t r = mus_r, w = mus_w;
        int32_t vol = (music_vol * (mus_fade >> 4)) >> 8;   // Q12
        for (uint32_t i = 0; i < FRAMES / HOLD; i++)
            acc[i] = r != w ? (mus_ring[r++ & (MUS_RING - 1u)] * vol) >> 12 : 0;
        mus_r = r;
        if (mus_fade_step) {
            int32_t f = mus_fade - mus_fade_step;
            if (f <= 0) { f = 0; mus_fade_step = 0; mus_on = 0; }
            mus_fade = f;
        }
        any = 1;
    }
    for (int c = 0; c < CHANNEL_COUNT; c++) {
        const int16_t *d = channels[c].data;
        if (!d) continue;
        uint32_t pos = channels[c].pos, end = channels[c].len << 16;
        const uint32_t step = (SFX_RATE << 16) / MIX_RATE;
        int32_t vol = channels[c].vol;
        uint32_t i = 0;
        if (!any) {
            for (; i < FRAMES / HOLD && pos < end; i++, pos += step) acc[i] = (d[pos >> 16] * vol) >> 12;
            for (uint32_t j = i; j < FRAMES / HOLD; j++) acc[j] = 0;
            any = 1;
        } else {
            for (; i < FRAMES / HOLD && pos < end; i++, pos += step) acc[i] += (d[pos >> 16] * vol) >> 12;
        }
        channels[c].pos = pos;
        if (pos >= end) channels[c].data = NULL;
    }
    if (!any) {
        AUDIO_PTR[half] = (uint32_t)silence - PSRAM_BASE;
        return;
    }
    for (uint32_t i = 0; i < FRAMES / HOLD; i++) {
        uint32_t v = (uint16_t)clamp16(acc[i]);
        v |= v << 16;
        out[2 * i] = v;
        out[2 * i + 1] = v;
    }
    cache_clean(out, sizeof(audio_buf[0]));
    AUDIO_PTR[half] = (uint32_t)out - PSRAM_BASE;
    sound_mixes++;
}

void sound_irq(uint32_t pending) {   // from the trap handler
    if (!(pending & (IRQ_AUDIO_A | IRQ_AUDIO_B))) return;
    uint32_t t0 = CLOCK_CNT;
    if (pending & IRQ_AUDIO_A) mix_half(0);
    if (pending & IRQ_AUDIO_B) mix_half(1);
    sound_cycles += CLOCK_CNT - t0;
}

// -------------------------------------------------------------------------
// loudness.h
// -------------------------------------------------------------------------
// The volume curve; set_volume() can come first (the configuration is
// loaded before init_audio()).
static void init_volume_table(void) {
    if (volume_table[255]) return;
    volume_table[0] = 0;
    for (int i = 1; i < 256; i++)
        volume_table[i] = (int32_t)(powf(10.0f, (255 - i) * (-30.0f / (20.0f * 255))) * 4096.0f);
}

bool init_audio(void) {
    if (audio_disabled) return false;
    init_volume_table();
    memset(silence, 0, sizeof(silence));
    cache_clean(silence, sizeof(silence));
    uint32_t m = irq_off();
    AUDIO_PTR[0] = (uint32_t)silence - PSRAM_BASE;
    AUDIO_PTR[1] = (uint32_t)silence - PSRAM_BASE;
    IRQ_PENDING = IRQ_AUDIO_A | IRQ_AUDIO_B;
    IRQ_ENABLE |= IRQ_AUDIO_A | IRQ_AUDIO_B;
    AUDIO_CTRL = 1u;
    irq_restore(m);
    return true;
}

void deinit_audio(void) {
    uint32_t m = irq_off();
    AUDIO_CTRL = 0u;
    IRQ_ENABLE &= ~(IRQ_AUDIO_A | IRQ_AUDIO_B);
    irq_restore(m);
    mus_close();
    memset(channels, 0, sizeof(channels));
}

void play_song(unsigned int song_num) {
    if (audio_disabled) return;
    if (song_num == song_playing && mus_missing) return;   // the game calls this every frame
    if (song_num == song_playing && mus_fd >= 0) {
        mus_fade_step = 0;
        mus_fade = 1 << 12;
        mus_on = 1;
        return;
    }
    mus_close();
    song_playing = song_num;
    songlooped = false;
    mus_missing = 0;
    char path[32];
    snprintf(path, sizeof(path), "TYRIAN/MUSIC/SONG%02u.WAV", song_num);
    if (mus_open_wav(path) < 0) {
        printf("music: no %s\n", path);
        mus_close();
        mus_missing = 1;
        return;
    }
    mus_eof = 0;
    mus_r = mus_w = 0;
    mus_fill(2);
    mus_fade_step = 0;
    mus_fade = 1 << 12;
    mus_on = !music_disabled;
    playing = mus_on;
}

void restart_song(void) {
    unsigned int s = song_playing;
    mus_close();
    song_playing = s;
    // reopen from the start
    char path[32];
    snprintf(path, sizeof(path), "TYRIAN/MUSIC/SONG%02u.WAV", s);
    if (mus_open_wav(path) < 0) { mus_close(); return; }
    songlooped = false;
    mus_eof = 0;
    mus_r = mus_w = 0;
    mus_fill(2);
    mus_fade_step = 0;
    mus_fade = 1 << 12;
    mus_on = !music_disabled;
}

void stop_song(void) {
    mus_on = 0;
    playing = false;
}

void fade_song(void) {
    // About 1.5 s (LDS fade speed 1: 63 volume steps at 70 Hz), per half (5.33 ms).
    if (mus_on && !mus_fade_step) mus_fade_step = (1 << 12) / 280;
}

void set_volume(Uint8 music, Uint8 sample) {
    init_volume_table();
    music_vol = volume_table[music] * 2;   // the renders are made at loudness.c's 2x
    sample_vol = volume_table[sample];
}

void multiSamplePlay(const Sint16 *samples, size_t count, Uint8 chan, Uint8 vol) {
    if (audio_disabled || samples_disabled || chan >= CHANNEL_COUNT || !samples) return;
    uint32_t m = irq_off();
    channels[chan].data = samples;
    channels[chan].len = (uint32_t)count;
    channels[chan].pos = 0;
    channels[chan].vol = sample_vol * (vol + 1) / CHANNEL_VOLUME_LEVELS;
    irq_restore(m);
}
