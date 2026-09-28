/*
 * main_host.c - headless test harness for the portable core.
 *
 * Runs a ROM for N frames and writes the last frame as PPM and the audio
 * as WAV. Used for development on Linux/macOS and for regression tests;
 * the same core sources are compiled for the Amiga with vbcc.
 *
 *   a26host rom.bin [-frames N] [-ppm out.ppm] [-wav out.wav]
 *           [-type F8|F6|...] [-region pal|ntsc] [-reset F] [-fire F]
 *           [-bench] [-q]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../core/atari.h"
#include "../core/palette.h"
#include "../core/cpu.h"
#include "../core/riot.h"

static u8 *load_file(const char *name, u32 *size)
{
    FILE *f = fopen(name, "rb");
    u8 *buf;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 1024 * 1024) { fclose(f); return NULL; }
    buf = (u8 *)malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *size = (u32)n;
    return buf;
}

static void write_ppm(const char *name, int first, int count)
{
    const u32 *pal = (a26.region == REGION_PAL) ? palette_pal : palette_ntsc;
    const u8 *fb = a26_framebuffer();
    FILE *f = fopen(name, "wb");
    int y, x;
    if (!f) { perror(name); return; }
    /* each TIA pixel is two pixels wide, like on the Amiga lowres screen */
    fprintf(f, "P6\n%d %d\n255\n", TIA_WIDTH * 2, count);
    for (y = first; y < first + count; y++) {
        for (x = 0; x < TIA_WIDTH; x++) {
            u32 c = 0;
            if (y >= 0 && y < TIA_FB_LINES) c = pal[fb[y * TIA_WIDTH + x] >> 1];
            {
                u8 px[6];
                px[0] = px[3] = (u8)(c >> 16);
                px[1] = px[4] = (u8)(c >> 8);
                px[2] = px[5] = (u8)c;
                fwrite(px, 1, 6, f);
            }
        }
    }
    fclose(f);
}

static void put_le(FILE *f, u32 v, int bytes)
{
    while (bytes--) { fputc((int)(v & 0xFF), f); v >>= 8; }
}

/* raw TIA colours of the frame just completed (for tools/refcompare.py):
 * "<lines>\n" followed by lines x 160 bytes */
static void write_raw(const char *prefix, int frame)
{
    char name[512];
    FILE *f;
    int lines = tia.frame_lines < TIA_FB_LINES ? tia.frame_lines : TIA_FB_LINES;
    sprintf(name, "%.480s%04d.raw", prefix, frame);
    f = fopen(name, "wb");
    if (!f) return;
    fprintf(f, "%d\n", lines);
    fwrite(a26_framebuffer(), 1, (size_t)lines * TIA_WIDTH, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *rom_name = NULL, *ppm = NULL, *wav = NULL, *raw = NULL;
    int raw_from = 0;
#ifdef A26_TRACE
    int trace_frame = -1;
#endif
    int frames = 120, bench = 0, quiet = 0, i;
    int reset_frame = -1, fire_frame = -1;
    CartType type = CART_UNKNOWN;
    int force_region = -1;
    u8 *rom;
    u32 size;
    FILE *wf = NULL;
    u32 wav_samples = 0;
    clock_t t0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "-wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "-type") && i + 1 < argc) type = cart_type_from_name(argv[++i]);
        else if (!strcmp(argv[i], "-region") && i + 1 < argc) {
            i++;
            force_region = !strcmp(argv[i], "pal") ? REGION_PAL : REGION_NTSC;
        }
        else if (!strcmp(argv[i], "-reset") && i + 1 < argc) reset_frame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-fire") && i + 1 < argc) fire_frame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-raw") && i + 1 < argc) raw = argv[++i];
        else if (!strcmp(argv[i], "-rawfrom") && i + 1 < argc) raw_from = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-clean")) riot_clean_start = 1;
#ifdef A26_TRACE
        else if (!strcmp(argv[i], "-trace") && i + 1 < argc) trace_frame = atoi(argv[++i]);
#endif
        else if (!strcmp(argv[i], "-bench")) bench = 1;
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (argv[i][0] != '-') rom_name = argv[i];
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (!rom_name) {
        fprintf(stderr, "usage: %s rom.bin [-frames N] [-ppm f] [-wav f] [-type T]"
                        " [-region pal|ntsc] [-reset F] [-fire F] [-raw prefix] [-rawfrom F] [-bench] [-q]\n", argv[0]);
        return 2;
    }
    rom = load_file(rom_name, &size);
    if (!rom) { fprintf(stderr, "cannot load %s\n", rom_name); return 1; }
    if (a26_load(rom, size, type)) {
        fprintf(stderr, "unsupported ROM (%lu bytes)\n", (unsigned long)size);
        return 1;
    }
    if (force_region >= 0) a26_force_region(force_region);
    a26_set_switches(0);

    if (wav) {
        wf = fopen(wav, "wb");
        /* reserve the header (AmigaDOS cannot seek past the end of a file) */
        if (wf) for (i = 0; i < 44; i++) fputc(0, wf);
    }

    t0 = clock();
    for (i = 0; i < frames; i++) {
        u8 sw = 0, joy = 0;
        if (reset_frame >= 0 && i >= reset_frame && i < reset_frame + 5) sw |= SW_RESET;
        if (fire_frame >= 0 && i >= fire_frame && i < fire_frame + 5) joy |= JOY_FIRE;
        a26_set_switches(sw);
        a26_set_joystick(0, joy);
#ifdef A26_TRACE
        { extern int tia_trace; tia_trace = (i == trace_frame); }
#endif
        a26_run_frame();
        if (raw && i >= raw_from) write_raw(raw, i);
        if (wf) {
            const u8 *s0, *s1;
            int n = a26_audio(0, &s0), k;
            a26_audio(1, &s1);
            for (k = 0; k < n; k++) {
                int v = (s0[k] + s1[k]) * 512 - 15360;   /* 0..60 -> signed 16 bit */
                put_le(wf, (u32)(v & 0xFFFF), 2);
            }
            wav_samples += (u32)n;
        }
        if (a26.region_changed) {
            a26.region_changed = 0;
            if (!quiet) printf("frame %d: region -> %s (%d lines)\n", i,
                               a26.region == REGION_PAL ? "PAL" : "NTSC", a26.lines_avg);
        }
    }

    if (bench) {
        /* integer maths: no floating point library needed on the Amiga */
        unsigned long ms = (unsigned long)((clock() - t0) * 1000 / CLOCKS_PER_SEC);
        unsigned long fps10 = ms ? (unsigned long)frames * 10000UL / ms : 0;
        printf("bench: %d frames in %lu ms = %lu.%lu fps\n", frames, ms, fps10 / 10, fps10 % 10);
    }

    printf("rom: %s  size: %lu  type: %s  region: %s  lines: %d (avg %d)  visible: %d-%d%s\n",
           rom_name, (unsigned long)size, cart_type_name(cart.type),
           a26.region == REGION_PAL ? "PAL" : "NTSC", tia.frame_lines, a26.lines_avg,
           tia.first_visible, tia.last_visible, cpu.jammed ? "  CPU JAMMED" : "");

    if (ppm) {
        int first = tia.first_visible, count = (a26.region == REGION_PAL) ? 256 : 200;
        if (first < 0) first = 0;
        /* centre the visible block in the Amiga screen height */
        if (tia.last_visible >= first) {
            int vis = tia.last_visible - first + 1;
            first -= (count - vis) / 2;
            if (first < 0) first = 0;
        }
        write_ppm(ppm, first, count);
    }

    if (wf) {
        u32 rate = (a26.region == REGION_PAL) ? 15600 : 15720;
        fseek(wf, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, wf); put_le(wf, 36 + wav_samples * 2, 4);
        fwrite("WAVEfmt ", 1, 8, wf); put_le(wf, 16, 4); put_le(wf, 1, 2); put_le(wf, 1, 2);
        put_le(wf, rate, 4); put_le(wf, rate * 2, 4); put_le(wf, 2, 2); put_le(wf, 16, 2);
        fwrite("data", 1, 4, wf); put_le(wf, wav_samples * 2, 4);
        fclose(wf);
    }
    free(rom);
    return 0;
}
