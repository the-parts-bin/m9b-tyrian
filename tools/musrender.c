// Renders Tyrian's music (music.mus: Loudness tunes for an OPL2) to WAV
// files for the m9b port, which streams them (port/loudness_m9b.c): each
// song once through, 24 kHz 16-bit mono, with OpenTyrian's own player
// (lds_play.c) and OPL emulator (opl.c) run as loudness.c runs them (a
// player update every 1/69.5 s), at loudness.c's 2x gain. A song ends when
// the player loops it (or stops, or after MAX_SECONDS). A song that stops
// instead of looping (the game then goes back to the level's song, say) gets an "m9b " chunk before its data, whose first byte is 1 (plays
// once); the port stops at its end rather than looping it.
//
// Build and run: see make_music.sh.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lds_play.h"
#include "opl.h"

int audioSampleRate = 24000;
#define MAX_SECONDS 600

static void put32(FILE *f, uint32_t v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
static void put16(FILE *f, uint16_t v) { fputc(v, f); fputc(v >> 8, f); }

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s music.mus OUTDIR\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    uint8_t *mus = malloc(total);
    fseek(f, 0, SEEK_SET);
    if (fread(mus, 1, total, f) != (size_t)total) { perror("read"); return 1; }
    fclose(f);

    unsigned count = mus[0] | mus[1] << 8;
    const int rate2 = 139;   // twice the update rate, as loudness.c
    int per = 2 * (audioSampleRate / rate2), per_frac = 2 * (audioSampleRate % rate2);
    int16_t *buf = malloc(sizeof(int16_t) * (size_t)audioSampleRate * MAX_SECONDS);

    for (unsigned s = 0; s < count; s++) {
        uint32_t pos = mus[2 + 4 * s] | mus[3 + 4 * s] << 8 | (uint32_t)mus[4 + 4 * s] << 16 | (uint32_t)mus[5 + 4 * s] << 24;
        uint32_t end = s + 1 < count ? (mus[6 + 4 * s] | mus[7 + 4 * s] << 8 | (uint32_t)mus[8 + 4 * s] << 16 | (uint32_t)mus[9 + 4 * s] << 24)
                                     : (uint32_t)total;
        opl_init();
        if (!lds_load(mus + pos, end - pos)) { fprintf(stderr, "song %u: load failed\n", s); continue; }
        size_t n = 0, max = (size_t)audioSampleRate * MAX_SECONDS;
        int until = 0, frac = 0, once = 0;
        while (n < max) {
            if (until == 0) {
                if (!lds_update() || songlooped) {
                    once = !playing;   // stopped (0xFC, or a hard fade) rather than looped
                    break;
                }
                until += per;
                frac += per_frac;
                if (frac >= rate2) { until++; frac -= rate2; }
            }
            int c = until;
            if ((size_t)c > max - n) c = (int)(max - n);
            opl_update(buf + n, c);
            n += (size_t)c;
            until -= c;
        }
        for (size_t i = 0; i < n; i++) {   // loudness.c's 2x ("OPL emulator is too quiet")
            int32_t v = buf[i] * 2;
            buf[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        char path[512];
        snprintf(path, sizeof(path), "%s/SONG%02u.WAV", argv[2], s);
        FILE *o = fopen(path, "wb");
        if (!o) { perror(path); return 1; }
        fwrite("RIFF", 1, 4, o); put32(o, 36 + (once ? 12 : 0) + 2 * (uint32_t)n); fwrite("WAVEfmt ", 1, 8, o);
        put32(o, 16); put16(o, 1); put16(o, 1); put32(o, audioSampleRate); put32(o, 2 * audioSampleRate);
        put16(o, 2); put16(o, 16);
        if (once) { fwrite("m9b ", 1, 4, o); put32(o, 4); put32(o, 1); }
        fwrite("data", 1, 4, o); put32(o, 2 * (uint32_t)n);
        fwrite(buf, 2, n, o);
        fclose(o);
        printf("song %2u: %5.1f s%s\n", s, (double)n / audioSampleRate, n >= max ? " (cut)" : once ? " (once)" : "");
    }
    return 0;
}
