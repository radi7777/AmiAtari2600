/*
 * main_amiga.c - AmiAtari2600 for AmigaOS 2.0+/3.x on 68030 + ECS.
 *
 * Usage (CLI):
 *   A26 <rom> [PAL|NTSC] [COLORS=PAL|NTSC] [TYPE=F8|F6|...] [SKIP=n]
 *             [DELAY=n] [PORT1] [NOSOUND] [PROFILE] [BENCH=n]
 *
 *   PAL / NTSC     force the region instead of detecting it
 *   COLORS=...     force the palette (e.g. PAL60 games: NTSC timing, PAL colours)
 *   TYPE=...       force the bankswitching scheme
 *   SKIP=n         render only every (n+1)th frame (default: automatic)
 *   DELAY=n        start emulating each frame n raster lines after the
 *                  vertical blank (default: automatic, DELAY=0 turns it off)
 *   LEFT=A|B       left difficulty switch at start (default B)
 *   RIGHT=A|B      right difficulty switch at start (default B)
 *   BW             TV type switch on B&W at start
 *   PORT1          use the joystick in the mouse port as player 2
 *   NOSOUND        no Paula output
 *   PRI=n          task priority while running (default 19, just below
 *                  input.device)
 *   KILLOS         switch off the OS interrupts while running (a little
 *                  faster on slow machines; the network may not survive)
 *   PROFILE        print timing statistics on exit
 *   BENCH=n        run n frames as fast as possible (no vsync, no frameskip),
 *                  then print the statistics and quit
 *   FRAMES=n       quit after n frames (normal speed; with PROFILE for tests)
 *
 * Keys: ESC quit, F1 Game Reset, F2 Game Select, F3 colour/B&W,
 *       F4/F5 left/right difficulty, F6 region auto/NTSC/PAL,
 *       P pause, HELP hard reset.
 *       Cursor keys + Space/Alt = player 1 joystick.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <proto/exec.h>

#include "hw.h"
#include "video.h"
#include "audio.h"
#include "input.h"
#include "../core/atari.h"
#include "../core/bus.h"

#define VERSION "0.1"

static const char vers[] = "$VER: A26 " VERSION " (" __DATE__ ")";

static struct {
    const char *rom;
    int region;         /* -1 auto */
    int colors;         /* -1 follow region */
    CartType type;
    int skip;           /* -1 auto */
    int delay;          /* -1 auto */
    int port1;
    int diff0, diff1;   /* difficulty switches at start: 1 = A */
    int bw;             /* B&W at start */
    int nosound;
    int killos;
    int nohandler;      /* diagnostics: no input.device handler */
    int notimer;        /* diagnostics: no timer.device sleeps */
    int profpc;         /* PC sampling profiler */
    int profile;
    long bench;         /* frames, 0 = off */
    long frames;        /* quit after n frames, 0 = never */
} opt;

static int display_pal;

/* ---- profiling (raster lines from CIA-B TOD) ---- */
enum { PH_DELAY, PH_EMU, PH_AUDIO, PH_RENDER, PH_WAIT, PH_COUNT };
static const char *const ph_name[PH_COUNT] = { "frame delay", "emulation", "audio", "render+c2p", "vsync wait" };
static struct {
    ULONG sum[PH_COUNT], max[PH_COUNT];
    ULONG frames, rendered, skipped, late;
    ULONG total_lines;
    ULONG lines_converted;
    ULONG chip_writes;
} prof;

static void prof_add(int ph, ULONG lines)
{
    prof.sum[ph] += lines;
    if (lines > prof.max[ph]) prof.max[ph] = lines;
}

static void prof_report(void)
{
    int i;
    ULONG budget = display_pal ? 312 : 262;
    if (!prof.frames) return;
    printf("\n%lu frames (%lu rendered, %lu skipped, %lu late), budget %lu lines/frame\n",
           prof.frames, prof.rendered, prof.skipped, prof.late, budget);
    printf("%-12s %8s %8s %8s\n", "phase", "avg", "max", "% budget");
    for (i = 0; i < PH_COUNT; i++) {
        ULONG n = (i == PH_RENDER || i == PH_WAIT || i == PH_DELAY) ? (prof.rendered ? prof.rendered : 1) : prof.frames;
        ULONG avg10 = prof.sum[i] * 10 / n;
        printf("%-12s %6lu.%lu %8lu %7lu%%\n", ph_name[i], avg10 / 10, avg10 % 10,
               prof.max[i], avg10 * 10 / budget);
    }
    printf("per rendered frame: %lu lines converted, %lu chip longwords written\n",
           prof.rendered ? prof.lines_converted / prof.rendered : 0,
           prof.rendered ? prof.chip_writes / prof.rendered : 0);
    if (prof.total_lines) {
        /* one raster line: 64 us (PAL), 63.5 us (NTSC) */
        ULONG ms = display_pal ? prof.total_lines * 64 / 1000 : prof.total_lines * 127 / 2000;
        printf("total %lu ms, %lu.%lu emulated frames/s\n", ms,
               ms ? prof.frames * 1000 / ms : 0, ms ? (prof.frames * 10000 / ms) % 10 : 0);
    }
}

/* PC samples: "offset count" per 16 byte bucket of the code hunk, for
 * tools/pcprof.py and the vlink map */
static void pc_report(void)
{
    ULONG i, n = prof_size / 16 + 1, total = prof_other;
    for (i = 0; i < n; i++) total += prof_hist[i];
    printf("PCPROF total %lu other %lu\n", total, prof_other);
    for (i = 0; i < n; i++)
        if (prof_hist[i]) printf("PC %lx %lu\n", i * 16, prof_hist[i]);
    FreeMem(prof_hist, n * 4);
}

static int vstart = -1, vstart_pending = -1, vstart_count;

static int streq_nocase(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

static int parse_args(int argc, char **argv)
{
    int i;
    opt.region = -1;
    opt.colors = -1;
    opt.type = CART_UNKNOWN;
    opt.skip = -1;
    opt.delay = -1;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (streq_nocase(a, "PAL")) opt.region = REGION_PAL;
        else if (streq_nocase(a, "NTSC")) opt.region = REGION_NTSC;
        else if (streq_nocase(a, "COLORS=PAL")) opt.colors = REGION_PAL;
        else if (streq_nocase(a, "COLORS=NTSC")) opt.colors = REGION_NTSC;
        else if (strncmp(a, "TYPE=", 5) == 0 || strncmp(a, "type=", 5) == 0) {
            opt.type = cart_type_from_name(a + 5);
            if (opt.type == CART_UNKNOWN) { printf("unknown cartridge type %s\n", a + 5); return -1; }
        }
        else if (strncmp(a, "SKIP=", 5) == 0 || strncmp(a, "skip=", 5) == 0) opt.skip = atoi(a + 5);
        else if (strncmp(a, "FRAMES=", 7) == 0 || strncmp(a, "frames=", 7) == 0) opt.frames = atol(a + 7);
        else if (strncmp(a, "DELAY=", 6) == 0 || strncmp(a, "delay=", 6) == 0) opt.delay = atoi(a + 6);
        else if (streq_nocase(a, "PORT1")) opt.port1 = 1;
        else if (streq_nocase(a, "LEFT=A")) opt.diff0 = 1;
        else if (streq_nocase(a, "LEFT=B")) opt.diff0 = 0;
        else if (streq_nocase(a, "RIGHT=A")) opt.diff1 = 1;
        else if (streq_nocase(a, "RIGHT=B")) opt.diff1 = 0;
        else if (streq_nocase(a, "BW")) opt.bw = 1;
        else if (streq_nocase(a, "NOSOUND")) opt.nosound = 1;
        else if (streq_nocase(a, "KILLOS")) opt.killos = 1;
        else if (strncmp(a, "PRI=", 4) == 0 || strncmp(a, "pri=", 4) == 0) hw_task_pri = atoi(a + 4);
        else if (streq_nocase(a, "NOHANDLER")) opt.nohandler = 1;
        else if (streq_nocase(a, "NOTIMER")) opt.notimer = 1;
        else if (streq_nocase(a, "PROFPC")) { opt.profpc = 1; opt.killos = 1; }
        else if (streq_nocase(a, "PROFILE")) opt.profile = 1;
        else if (strncmp(a, "BENCH=", 6) == 0 || strncmp(a, "bench=", 6) == 0) {
            opt.bench = atol(a + 6);
            opt.profile = 1;
            opt.skip = 0;
            opt.delay = 0;
        }
        else if (!opt.rom) opt.rom = a;
        else { printf("unknown option %s\n", a); return -1; }
    }
    if (!opt.rom) {
        printf("A26 %s - Atari 2600 emulator for Amiga 68030/ECS\n"
               "usage: A26 <rom> [PAL|NTSC] [COLORS=PAL|NTSC] [TYPE=F8|F6|F4|...]\n"
               "           [SKIP=n] [DELAY=n] [PORT1] [LEFT=A|B] [RIGHT=A|B] [BW] [NOSOUND] [KILLOS]\n"
               "           [PRI=n] [PROFILE] [BENCH=n] [FRAMES=n]\n", VERSION);
        return -1;
    }
    return 0;
}

static u8 *load_rom(const char *name, u32 *size)
{
    FILE *f = fopen(name, "rb");
    u8 *buf = NULL;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0 && n <= 512L * 1024L) {
        buf = (u8 *)malloc((size_t)n);
        if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    }
    fclose(f);
    *size = (u32)n;
    return buf;
}

/* program display + audio for the current emulated region */
static void apply_mode(void)
{
    int want_pal = (a26.region == REGION_PAL);
    int colours = (opt.colors >= 0) ? opt.colors : want_pal;

    hw_wait_vbl();
    display_pal = hw_set_pal(want_pal);
    video_set_mode(display_pal, colours);
    if (!opt.nosound) audio_start(display_pal);
    vstart = -1;
}

/* choose the first TIA line shown, centring the game's visible area.
 * Changes are only taken over when stable, so games that vary their
 * VBLANK length slightly do not make the picture jump. */
static int choose_vstart(void)
{
    int h = video_height();
    int first = tia.first_visible, last = tia.last_visible, want;

    if (first < 0 || last < first) {
        want = (a26.region == REGION_PAL) ? 48 : 36;
    } else {
        int vis = last - first + 1;
        want = (vis >= h) ? first : first - (h - vis) / 2;
    }
    if (want > TIA_FB_LINES - h) want = TIA_FB_LINES - h;
    if (want < 0) want = 0;

    if (vstart < 0) {
        vstart = want;
    } else if (want != vstart) {
        if (want == vstart_pending) {
            if (++vstart_count >= 30) vstart = want;
        } else {
            vstart_pending = want;
            vstart_count = 0;
        }
    }
    return vstart;
}

/* ---- input ----
 * The keyboard is read once per frame (it needs a handshake), the
 * joysticks whenever the emulated program reads SWCHA/INPT4/INPT5, at
 * most once per emulated scanline. Together with the frame delay below
 * this keeps the time from joystick to screen as short as possible. */
static u8  kbd_joy0;
static u32 joy_cycle;
static int joy_fresh;

static void poll_joysticks(void)
{
    a26_set_joystick(0, (u8)(joy_read(1) | kbd_joy0));
    if (opt.port1) a26_set_joystick(1, joy_read(0));
}

static void input_hook(void)
{
    if (joy_fresh && a26_cycles - joy_cycle < 76) return;
    joy_cycle = a26_cycles;
    joy_fresh = 1;
    poll_joysticks();
}

/* ---- frame delay ----
 * Emulating a frame takes only part of a display frame on a fast machine.
 * Instead of starting right after the vertical blank and then idling, we
 * start as late as possible, so input is read shortly before the frame is
 * shown. The start line follows the slowest of the last 128 frames plus a
 * margin; a missed vertical blank adds extra margin for the next frames. */
#define WORK_HIST 128
static int   delay_line;
static ULONG work_hist[WORK_HIST];
static int   work_pos;
static ULONG late_bonus;

static void delay_update(ULONG work, int late)
{
    int budget = display_pal ? 312 : 262;
    ULONG peak = 0;
    int i, d;
    if (opt.delay >= 0) {
        delay_line = opt.delay;
        return;
    }
    if (work > (ULONG)budget * 2)
        return;                         /* bogus measurement */
    work_hist[work_pos] = work;
    work_pos = (work_pos + 1) % WORK_HIST;
    for (i = 0; i < WORK_HIST; i++)
        if (work_hist[i] > peak) peak = work_hist[i];
    if (late)
        late_bonus = (ULONG)budget / 4;  /* missed the vertical blank */
    else if (late_bonus)
        late_bonus--;
    d = budget - (int)(peak + peak / 4 + late_bonus) - 24;
    delay_line = d > 0 ? d : 0;
}

static void delay_wait(void)
{
    /* the vertical blank interrupt comes at line 0; never waits across it */
    hw_wait_line(delay_line);
}

static void run(void)
{
    u8 switches = 0;
    int bw = opt.bw, diff0 = opt.diff0, diff1 = opt.diff1, paused = 0;
    int skip_count = 0, late = 0;

    ULONG t, t_start, t_work;

    input_init(opt.killos || opt.nohandler ? -1 : 0);
    apply_mode();
    a26_input_hook = input_hook;
    t_start = hw_lines();

    for (;;) {
        if (opt.bench && (long)prof.frames >= opt.bench) break;
        if (opt.frames && (long)prof.frames >= opt.frames) break;

        t = hw_lines();
        if (!late && skip_count == 0) delay_wait();
        prof_add(PH_DELAY, hw_lines_since(t));
        t_work = hw_lines();

        input_poll();
        if (key_down(KEY_ESC)) break;
        if (key_pressed(KEY_P)) {
            paused = !paused;
            if (!opt.nosound) { if (paused) audio_stop(); else audio_start(display_pal); }
        }
        if (key_pressed(KEY_HELP)) a26_reset();
        if (key_pressed(KEY_F3)) bw = !bw;
        if (key_pressed(KEY_F4)) diff0 = !diff0;
        if (key_pressed(KEY_F5)) diff1 = !diff1;
        if (key_pressed(KEY_F6)) {
            /* auto -> NTSC -> PAL -> auto */
            if (!a26.region_forced) a26_force_region(REGION_NTSC);
            else if (a26.region == REGION_NTSC) a26_force_region(REGION_PAL);
            else a26_force_region(-1);
        }
        if (paused) { hw_wait_vbl(); continue; }

        switches = 0;
        if (key_down(KEY_F1)) switches |= SW_RESET;
        if (key_down(KEY_F2)) switches |= SW_SELECT;
        if (bw) switches |= SW_BW;
        if (diff0) switches |= SW_DIFF_P0;
        if (diff1) switches |= SW_DIFF_P1;
        a26_set_switches(switches);

        kbd_joy0 = 0;
        if (key_down(KEY_UP)) kbd_joy0 |= JOY_UP;
        if (key_down(KEY_DOWN)) kbd_joy0 |= JOY_DOWN;
        if (key_down(KEY_LEFT)) kbd_joy0 |= JOY_LEFT;
        if (key_down(KEY_RIGHT)) kbd_joy0 |= JOY_RIGHT;
        if (key_down(KEY_SPACE) || key_down(KEY_LALT) || key_down(KEY_RALT)) kbd_joy0 |= JOY_FIRE;
        poll_joysticks();
        joy_fresh = 0;

        t = hw_lines();
        a26_run_frame();
        video_note_frame();
        prof_add(PH_EMU, hw_lines_since(t));
        prof.frames++;

        if (a26.region_changed) {
            a26.region_changed = 0;
            apply_mode();
        }
        t = hw_lines();
        if (!opt.nosound) audio_frame();
        prof_add(PH_AUDIO, hw_lines_since(t));

        /* frame skipping: fixed (SKIP=n) or automatic when we missed the
         * previous vertical blank (at most 2 frames in a row) */
        if ((opt.skip > 0 && skip_count < opt.skip) ||
            (opt.skip < 0 && late && skip_count < 2)) {
            skip_count++;
            prof.skipped++;
            late = 0;
            continue;
        }
        skip_count = 0;
        t = hw_lines();
        video_render(a26_framebuffer(), TIA_FB_LINES, choose_vstart());
        prof_add(PH_RENDER, hw_lines_since(t));
        prof.lines_converted += video_stat_lines;
        prof.chip_writes += video_stat_writes;
        t = hw_lines();
        late = video_present(!opt.bench);
        delay_update(hw_lines_diff(t, t_work), late);
        prof_add(PH_WAIT, hw_lines_since(t));
        prof.rendered++;
        if (late) prof.late++;
    }
    a26_input_hook = 0;
    input_cleanup();
    prof.total_lines = hw_lines_since(t_start);
}

int main(int argc, char **argv)
{
    u8 *rom;
    u32 size;
    int err, i;

    (void)vers;
    if (argc == 0) return 0;                 /* started from Workbench: not yet supported */
    if (parse_args(argc, argv)) return 5;

    rom = load_rom(opt.rom, &size);
    if (!rom) { printf("cannot load %s\n", opt.rom); return 10; }
    err = a26_load(rom, size, opt.type);
    if (err) { printf("unsupported ROM (%lu bytes, error %d)\n", (unsigned long)size, err); free(rom); return 10; }
    if (opt.region >= 0) a26_force_region(opt.region);

    if (hw_init()) { printf("cannot open graphics.library\n"); free(rom); return 20; }

    /* run a few frames blind to detect PAL/NTSC before opening the display */
    for (i = 0; i < 20; i++) a26_run_frame();
    a26.region_changed = 0;

    printf("A26 %s: %s, %lu bytes, %s, %s (%d lines)%s\n", VERSION, opt.rom,
           (unsigned long)size, cart_type_name(cart.type),
           a26.region == REGION_PAL ? "PAL" : "NTSC", a26.lines_avg,
           hwinfo.ecs ? "" : "\n  note: OCS Agnus, no PAL/NTSC switching (needs 8372A)");

    if (video_init()) { printf("not enough chip memory\n"); hw_cleanup(); free(rom); return 20; }
    if (!opt.nosound && audio_init()) opt.nosound = 1;

    a26_reset();
    hw_no_timer = opt.notimer;
    hw_takeover(opt.killos);
    if (opt.profpc && hw_profile_start()) opt.profpc = 0;
    run();
    if (opt.profpc) hw_profile_stop();
    if (!opt.nosound) audio_stop();
    hw_restore();
    if (opt.profile) prof_report();
    if (opt.profpc) pc_report();

    audio_free();
    video_free();
    hw_cleanup();
    free(rom);
    return 0;
}
