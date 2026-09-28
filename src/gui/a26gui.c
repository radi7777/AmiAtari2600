/*
 * a26gui.c - MUI launcher for the A26 emulator.
 *
 *   A26GUI [ROMDIR=<dir>]
 *
 * Game list with filter, screenshot preview, game information and the
 * console switches (saved per game). The information and screenshots are
 * scraped on the Amiga itself with curl from the libretro databases and
 * thumbnails and cached in PROGDIR:db and PROGDIR:snaps. "Spielen" runs
 * PROGDIR:A26 with the game's switches and returns to the list.
 *
 * On an RTG Workbench the window opens there; on a native display the
 * launcher opens its own screen like the Workbench with more colours
 * (the Workbench often runs with 8 colours only).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/displayinfo.h>
#include <graphics/scale.h>
#include <datatypes/datatypes.h>
#include <datatypes/pictureclass.h>
#include <libraries/asl.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/datatypes.h>
#include <proto/utility.h>
#include <clib/alib_protos.h>

#include "muistubs.h"
#include "gamedb.h"
#include "snapimg.h"
#include "../core/cart.h"

#define VERSION "0.1"
static const char vers[] = "$VER: A26GUI " VERSION " (" __DATE__ ")";

struct Library *MUIMasterBase;
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *DataTypesBase;
struct Library *UtilityBase;

/* ---- trace log (PROGDIR:A26GUI.log), opened for every line so it can
 * be read while the program runs and survives a crash; enabled with
 * DEBUG ---- */
static int debug;
static void trace(const char *fmt, const char *arg)
{
    FILE *f;
    if (!debug) return;
    f = fopen("PROGDIR:A26GUI.log", "a");
    if (!f) return;
    fprintf(f, fmt, arg ? arg : "");
    fputc('\n', f);
    fclose(f);
}

/* ---- varargs helpers for the MUI macros ---- */
Object *MUI_NewObject(CONST_STRPTR cl, Tag tag1, ...)
{
    return MUI_NewObjectA(cl, (struct TagItem *)&tag1);
}

Object *MUI_MakeObject(LONG type, ...)
{
    return MUI_MakeObjectA(type, (ULONG *)(&type + 1));
}

/* ---- games ---- */
typedef struct {
    char  path[256];
    char  name[128];            /* display name */
    crc_t crc;
    long  size;
    const GameInfo *info;
    /* console switches (saved per game) */
    UBYTE diff0, diff1;         /* 1 = A */
    UBYTE bw;
    UBYTE region;               /* 0 auto, 1 NTSC, 2 PAL */
    UBYTE colors;               /* 0 auto, 1 NTSC, 2 PAL */
    UBYTE port1;                /* player 2 on the mouse port joystick */
} Game;

static Game *games;
static int   ngames;
static int  *shown;             /* list position -> game index */
static int   nshown;
static char  romdir[256] = "PROGDIR:roms";

#define PREFS_FILE  "PROGDIR:A26GUI.prefs"
#define GAMES_FILE  "PROGDIR:A26.games"
#define DB_DIR      "PROGDIR:db"
#define SNAP_DIR    "PROGDIR:snaps"

/* MUI needs more than the 4 KB a Shell gives by default (with too little
 * the window never opened on a chipset screen) */
size_t __stack = 65536;

/* PROGDIR: only means our directory to us; paths handed to other programs
 * (curl, A26) must be absolute */
static char progdir[256];

static void abs_path(const char *p, char *out, int outlen)
{
    if (!strncmp(p, "PROGDIR:", 8) && progdir[0]) {
        strncpy(out, progdir, outlen - 1);
        out[outlen - 1] = 0;
        AddPart((STRPTR)out, (STRPTR)(p + 8), outlen);
    } else {
        strncpy(out, p, outlen - 1);
        out[outlen - 1] = 0;
    }
}

/* ---- MUI objects ---- */
static Object *app, *win, *lv_games, *lst_games, *str_filter, *txt_count, *txt_info;
static Object *img, *cy_diff0, *cy_diff1, *cy_tv, *cy_region, *cy_colors, *cy_port;
static Object *bt_start, *bt_romdir, *bt_db, *bt_snaps, *bt_quit;
static struct Screen *own_screen;

enum { ID_SELECT = 1, ID_START, ID_FILTER, ID_SWITCH, ID_ROMDIR, ID_DB, ID_SNAPS };

/* ---------------------------------------------------------------------
 * screenshots: converted once from the downloaded PNG into a small cache
 * file (snapimg.c: border cut off, at most 320 x 240, palette + packed
 * indices), so browsing the list never decodes a PNG again.
 */

/* WritePixelArray() of cybergraphics.library (RTG screens), if present */
static struct Library *CyberGfxBase;
ULONG __WritePixelArray(__reg("a6") struct Library *, __reg("a0") APTR src,
                        __reg("d0") UWORD sx, __reg("d1") UWORD sy, __reg("d2") UWORD mod,
                        __reg("a1") struct RastPort *rp, __reg("d3") UWORD dx,
                        __reg("d4") UWORD dy, __reg("d5") UWORD w, __reg("d6") UWORD h,
                        __reg("d7") UBYTE fmt) = "\tjsr\t-126(a6)";
#define RECTFMT_RGB 0

static int dt_getrow(void *ctx, int y, unsigned char *rgb)
{
    Object *dto = (Object *)((ULONG *)ctx)[0];
    ULONG w = ((ULONG *)ctx)[1];
    return DoMethod(dto, PDTM_READPIXELARRAY, (ULONG)rgb, PBPAFMT_RGB, w * 3, 0, y, w, 1) ? 0 : -1;
}

/* PNG (any datatype picture) -> cache file; returns 0 on success */
static int snap_convert(const char *png, const char *cache)
{
    static struct TagItem tags[] = {
        { DTA_GroupID, GID_PICTURE }, { PDTA_Remap, FALSE }, { PDTA_DestMode, PMODE_V43 },
        { TAG_DONE, 0 }
    };
    struct BitMapHeader *bmh = NULL;
    struct TagItem gt[2];
    Object *dto;
    SnapImg im;
    ULONG ctx[2];
    int err = -1;
    trace("convert %s", png);
    dto = NewDTObjectA((APTR)png, tags);
    if (!dto) return -1;
    gt[0].ti_Tag = PDTA_BitMapHeader; gt[0].ti_Data = (ULONG)&bmh;
    gt[1].ti_Tag = TAG_DONE;
    GetDTAttrsA(dto, gt);
    if (bmh) {
        ctx[0] = (ULONG)dto;
        ctx[1] = bmh->bmh_Width;
        if (!snapimg_from_rgb(&im, bmh->bmh_Width, bmh->bmh_Height, dt_getrow, ctx)) {
            err = snapimg_save(cache, &im);
            snapimg_free(&im);
        }
    }
    DisposeDTObject(dto);
    if (err) DeleteFile((STRPTR)cache);
    trace(err ? "convert failed" : "convert ok", NULL);
    return err;
}

/* ---------------------------------------------------------------------
 * screenshot area: a small custom class showing a cached screenshot
 * (MUIA_A26Img_File = .a26i file) scaled to the area, keeping its aspect
 * (also for screens with non-square pixels), centred on black.
 */
#define MUIA_A26Img_File (TAG_USER | 0x26000001)

struct ImgData {
    SnapImg si;                 /* the picture, si.pix == NULL: none */
    struct BitMap *sbm;         /* scaled for the screen */
    LONG sw, sh, aw, ah;        /* its size, for an area of aw x ah */
    LONG black;                 /* pen for the area background, -1 if none */
    LONG rx, ry;                /* pixel aspect of the screen (display ticks) */
    int setup;
    int shown;                  /* between MUIM_Show and MUIM_Hide */
    int clut;                   /* screen with a colour table (<= 8 planes) */
    struct ColorMap *cm;
    struct BitMap *scrbm;
    int npens;                  /* pens obtained for the palette */
    LONG pen[256];              /* palette index -> pen, -1 not yet */
    char file[256];
};

static struct MUI_CustomClass *img_class;

static void img_free_scaled(struct ImgData *d)
{
    int i;
    if (d->sbm) { WaitBlit(); FreeBitMap(d->sbm); }
    d->sbm = NULL;
    if (d->npens) {
        for (i = 0; i < 256; i++)
            if (d->pen[i] >= 0) ReleasePen(d->cm, d->pen[i]);
        d->npens = 0;
    }
    for (i = 0; i < 256; i++) d->pen[i] = -1;
}

static void img_free(struct ImgData *d)
{
    img_free_scaled(d);
    snapimg_free(&d->si);
}

/* pen for palette entry i (obtained on first use) */
static UBYTE img_pen(struct ImgData *d, int i)
{
    static struct TagItem obp[] = { { OBP_Precision, PRECISION_IMAGE }, { TAG_DONE, 0 } };
    if (d->pen[i] < 0) {
        const UBYTE *c = d->si.pal + i * 3;
        d->pen[i] = ObtainBestPenA(d->cm, c[0] * 0x01010101UL, c[1] * 0x01010101UL,
                                   c[2] * 0x01010101UL, obp);
        d->npens++;
    }
    return (UBYTE)d->pen[i];
}

/* picture scaled to fit aw x ah (nearest neighbour on the indices) */
static void img_scale(struct ImgData *d, LONG aw, LONG ah)
{
    struct RastPort rp;
    UBYTE *buf;
    LONG dw, dh, x, y, rgb;
    if (d->sbm && d->aw == aw && d->ah == ah) return;
    img_free_scaled(d);
    d->aw = aw; d->ah = ah;
    /* the snaps have square pixels; the screen's may not (hires PAL:
     * twice as high as wide) */
    dh = ah;
    dw = d->si.w * ah * d->ry / (d->si.h * d->rx);
    if (dw > aw) {
        dw = aw;
        dh = d->si.h * aw * d->rx / (d->si.w * d->ry);
    }
    if (dw < 1 || dh < 1) return;
    rgb = !d->clut && CyberGfxBase;
    buf = (UBYTE *)AllocVec(dw * dh * (rgb ? 3 : 1), MEMF_ANY);
    d->sbm = AllocBitMap(dw, dh, GetBitMapAttr(d->scrbm, BMA_DEPTH), BMF_CLEAR, d->scrbm);
    if (buf && d->sbm) {
        UBYTE *o = buf;
        for (y = 0; y < dh; y++) {
            const UBYTE *src = d->si.pix + (y * d->si.h / dh) * d->si.w;
            for (x = 0; x < dw; x++) {
                int i = src[x * d->si.w / dw];
                if (rgb) {
                    const UBYTE *c = d->si.pal + i * 3;
                    *o++ = c[0]; *o++ = c[1]; *o++ = c[2];
                } else {
                    *o++ = img_pen(d, i);
                }
            }
        }
        InitRastPort(&rp);
        rp.BitMap = d->sbm;
        if (rgb)
            __WritePixelArray(CyberGfxBase, buf, 0, 0, dw * 3, &rp, 0, 0, dw, dh, RECTFMT_RGB);
        else
            WriteChunkyPixels(&rp, 0, 0, dw - 1, dh - 1, buf, dw);
        d->sw = dw;
        d->sh = dh;
    } else if (d->sbm) {
        FreeBitMap(d->sbm);
        d->sbm = NULL;
    }
    if (buf) FreeVec(buf);
}

static void img_load(struct ImgData *d)
{
    img_free(d);
    if (!d->file[0]) return;
    if (snapimg_load(d->file, &d->si)) trace("image: cannot load %s", d->file);
}

/* returns whether the picture changed */
static int img_set(struct ImgData *d, struct TagItem *tags)
{
    struct TagItem *t = FindTagItem(MUIA_A26Img_File, tags);
    if (!t) return 0;
    if (t->ti_Data) {
        strncpy(d->file, (const char *)t->ti_Data, sizeof(d->file) - 1);
        d->file[sizeof(d->file) - 1] = 0;
    } else {
        d->file[0] = 0;
    }
    img_load(d);
    return 1;
}

/* __saveds: MUI calls this with its own a4, and our globals (library
 * bases included) are addressed through a4 in vbcc's small data model */
static __saveds ULONG img_dispatch(__reg("a0") struct IClass *cl, __reg("a2") Object *obj,
                                   __reg("a1") Msg msg)
{
    struct ImgData *d;
    switch (msg->MethodID) {
    case OM_NEW:
        obj = (Object *)DoSuperMethodA(cl, obj, msg);
        if (obj) {
            d = INST_DATA(cl, obj);
            memset(d, 0, sizeof(*d));
            d->black = -1;
            memset(d->pen, 0xFF, sizeof(d->pen));
            img_set(d, ((struct opSet *)msg)->ops_AttrList);
        }
        return (ULONG)obj;
    case OM_DISPOSE:
        img_free(INST_DATA(cl, obj));
        break;
    case OM_SET:
        /* MUI sets attributes of its own too, also while the window is
         * being opened: draw only for a new picture and only when shown */
        d = INST_DATA(cl, obj);
        if (img_set(d, ((struct opSet *)msg)->ops_AttrList) && d->shown)
            MUI_Redraw(obj, MADF_DRAWOBJECT);
        break;
    case MUIM_Show:
        if (!DoSuperMethodA(cl, obj, msg)) return FALSE;
        d = INST_DATA(cl, obj);
        d->shown = 1;
        return TRUE;
    case MUIM_Hide:
        d = INST_DATA(cl, obj);
        d->shown = 0;
        break;
    case MUIM_Setup:
        trace("image: setup", NULL);
        if (!DoSuperMethodA(cl, obj, msg)) return FALSE;
        d = INST_DATA(cl, obj);
        d->setup = 1;
        trace("image: pens", NULL);
        d->cm = _screen(obj)->ViewPort.ColorMap;
        d->scrbm = _screen(obj)->RastPort.BitMap;
        d->clut = GetBitMapAttr(d->scrbm, BMA_DEPTH) <= 8;
        d->black = ObtainBestPenA(d->cm, 0, 0, 0, NULL);
        {
            struct DisplayInfo di;
            ULONG mode = GetVPModeID(&_screen(obj)->ViewPort);
            d->rx = d->ry = 1;
            if (mode != INVALID_ID &&
                GetDisplayInfoData(NULL, (UBYTE *)&di, sizeof(di), DTAG_DISP, mode) &&
                di.Resolution.x > 0 && di.Resolution.y > 0) {
                d->rx = di.Resolution.x;
                d->ry = di.Resolution.y;
            }
        }
        return TRUE;
    case MUIM_Cleanup:
        d = INST_DATA(cl, obj);
        img_free_scaled(d);     /* pens and bitmap belong to the screen */
        if (d->black >= 0) ReleasePen(_screen(obj)->ViewPort.ColorMap, d->black);
        d->black = -1;
        d->setup = 0;
        break;
    case MUIM_AskMinMax: {
        struct MUI_MinMax *mm;
        DoSuperMethodA(cl, obj, msg);
        mm = ((struct MUIP_AskMinMax *)msg)->MinMaxInfo;
        mm->MinWidth += 160; mm->DefWidth += 320; mm->MaxWidth += 320;
        mm->MinHeight += 80; mm->DefHeight += 240; mm->MaxHeight += 240;
        return 0;
    }
    case MUIM_Draw:
        DoSuperMethodA(cl, obj, msg);
        d = INST_DATA(cl, obj);
        if ((((struct MUIP_Draw *)msg)->flags & MADF_DRAWOBJECT)) {
            LONG x = _mleft(obj), y = _mtop(obj), w = _mwidth(obj), h = _mheight(obj);
            SetAPen(_rp(obj), d->black >= 0 ? d->black : _dri(obj)->dri_Pens[BACKGROUNDPEN]);
            RectFill(_rp(obj), x, y, x + w - 1, y + h - 1);
            if (d->si.pix && d->setup) img_scale(d, w, h);
            if (d->sbm) {
                LONG bw = d->sw < w ? d->sw : w, bh = d->sh < h ? d->sh : h;
                BltBitMapRastPort(d->sbm, 0, 0, _rp(obj), x + (w - bw) / 2, y + (h - bh) / 2,
                                  bw, bh, 0xC0);
            }
        }
        return 0;
    }
    return DoSuperMethodA(cl, obj, msg);
}

/* ---------------------------------------------------------------------
 * helpers
 */
static int exists(const char *path)
{
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    if (l) { UnLock(l); return 1; }
    return 0;
}

static void make_dir(const char *path)
{
    BPTR l = Lock((STRPTR)path, ACCESS_READ);
    if (!l) l = CreateDir((STRPTR)path);
    if (l) UnLock(l);
}

static int run(const char *cmd)
{
    BPTR in = Open((STRPTR)"NIL:", MODE_OLDFILE);
    BPTR out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    struct TagItem t[3];
    LONG rc;
    t[0].ti_Tag = SYS_Input;  t[0].ti_Data = (ULONG)in;
    t[1].ti_Tag = SYS_Output; t[1].ti_Data = (ULONG)out;
    t[2].ti_Tag = TAG_DONE;
    trace("run: %s", cmd);
    rc = SystemTagList((STRPTR)cmd, t);
    trace("run: done", NULL);
    if (rc == -1) { Close(in); Close(out); }     /* not started: still ours */
    return (int)rc;
}

static void set_info(const char *s)
{
    set(txt_info, MUIA_Text_Contents, s);
}

static int icase_has(const char *hay, const char *needle)
{
    int n = (int)strlen(needle), i, k;
    if (!n) return 1;
    for (i = 0; hay[i]; i++) {
        for (k = 0; k < n; k++) {
            char a = hay[i + k], b = needle[k];
            if (!a) return 0;
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (k == n) return 1;
    }
    return 0;
}

/* ---- settings ---- */
static void load_prefs(void)
{
    FILE *f = fopen(PREFS_FILE, "r");
    char line[300];
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *e = strchr(line, '\n');
        if (e) *e = 0;
        if (!strncmp(line, "romdir=", 7)) strncpy(romdir, line + 7, sizeof(romdir) - 1);
    }
    fclose(f);
}

static void save_prefs(void)
{
    FILE *f = fopen(PREFS_FILE, "w");
    if (!f) return;
    fprintf(f, "romdir=%s\n", romdir);
    fclose(f);
}

static void load_switches(void)
{
    FILE *f = fopen(GAMES_FILE, "r");
    char line[128];
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        unsigned long crc;
        int d0, d1, bw, re, co, p1, i;
        if (sscanf(line, "%lx %d %d %d %d %d %d", &crc, &d0, &d1, &bw, &re, &co, &p1) != 7) continue;
        for (i = 0; i < ngames; i++)
            if (games[i].crc == crc) {
                games[i].diff0 = (UBYTE)d0; games[i].diff1 = (UBYTE)d1; games[i].bw = (UBYTE)bw;
                games[i].region = (UBYTE)re; games[i].colors = (UBYTE)co; games[i].port1 = (UBYTE)p1;
            }
    }
    fclose(f);
}

static void save_switches(void)
{
    FILE *f = fopen(GAMES_FILE, "w");
    int i;
    if (!f) return;
    for (i = 0; i < ngames; i++) {
        Game *g = &games[i];
        if (g->diff0 | g->diff1 | g->bw | g->region | g->colors | g->port1)
            fprintf(f, "%08lX %d %d %d %d %d %d\n", (unsigned long)g->crc, (int)g->diff0,
                    (int)g->diff1, (int)g->bw, (int)g->region, (int)g->colors, (int)g->port1);
    }
    fclose(f);
}

/* ---- game list ---- */
static int cmp_game(const void *a, const void *b)
{
    const char *x = ((const Game *)a)->name, *y = ((const Game *)b)->name;
    for (;; x++, y++) {
        char p = *x, q = *y;
        if (p >= 'a' && p <= 'z') p -= 32;
        if (q >= 'a' && q <= 'z') q -= 32;
        if (p != q || !p) return p - q;
    }
}

static void scan_roms(void)
{
    struct FileInfoBlock *fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    BPTR dir = Lock((STRPTR)romdir, ACCESS_READ);
    static unsigned char buf[65536];
    int cap = 0;

    free(games);
    games = NULL;
    ngames = 0;
    if (!fib || !dir) {
        if (dir) UnLock(dir);
        if (fib) FreeDosObject(DOS_FIB, fib);
        return;
    }
    if (Examine(dir, fib)) {
        while (ExNext(dir, fib)) {
            char path[256];
            const char *n = (const char *)fib->fib_FileName;
            size_t l = strlen(n);
            FILE *f;
            long size;
            Game *g;
            if (fib->fib_DirEntryType > 0) continue;
            if (fib->fib_Size < 2048 || fib->fib_Size > 65536) continue;
            if (l < 4 || !(icase_has(n + l - 4, ".bin") || icase_has(n + l - 4, ".a26") ||
                           icase_has(n + l - 4, ".rom")))
                continue;
            strcpy(path, romdir);
            if (path[strlen(path) - 1] != ':' && path[strlen(path) - 1] != '/') strcat(path, "/");
            strncat(path, n, sizeof(path) - strlen(path) - 1);
            f = fopen(path, "rb");
            if (!f) continue;
            size = (long)fread(buf, 1, sizeof(buf), f);
            fclose(f);
            if (ngames == cap) {
                Game *ng;
                cap = cap ? cap * 2 : 64;
                ng = (Game *)realloc(games, cap * sizeof(Game));
                if (!ng) break;
                games = ng;
            }
            g = &games[ngames++];
            memset(g, 0, sizeof(*g));
            strcpy(g->path, path);
            g->size = size;
            g->crc = crc32_buf(0, buf, size);
            g->info = gamedb_find(g->crc);
            if (g->info && g->info->name) strncpy(g->name, g->info->name, sizeof(g->name) - 1);
            else strncpy(g->name, n, sizeof(g->name) - 1);
        }
    }
    UnLock(dir);
    FreeDosObject(DOS_FIB, fib);
    if (ngames) qsort(games, ngames, sizeof(Game), cmp_game);
    free(shown);
    shown = (int *)malloc((ngames + 1) * sizeof(int));
    load_switches();
}

static void refresh_info(void);

static void fill_list(void)
{
    char *filter = NULL;
    char buf[64];
    int i;
    get(str_filter, MUIA_String_Contents, &filter);
    set(lst_games, MUIA_List_Quiet, TRUE);
    DoMethod(lst_games, MUIM_List_Clear);
    nshown = 0;
    for (i = 0; i < ngames; i++) {
        if (filter && !icase_has(games[i].name, filter)) continue;
        shown[nshown++] = i;
        DoMethod(lst_games, MUIM_List_InsertSingle, games[i].name, MUIV_List_Insert_Bottom);
    }
    set(lst_games, MUIA_List_Quiet, FALSE);
    sprintf(buf, "%d Spiele", nshown);
    set(txt_count, MUIA_Text_Contents, buf);
    set(lst_games, MUIA_List_Active, nshown ? 0 : MUIV_List_Active_Off);
    refresh_info();
}

static Game *current(void)
{
    LONG a = MUIV_List_Active_Off;
    get(lst_games, MUIA_List_Active, &a);
    if (a < 0 || a >= nshown) return NULL;
    return &games[shown[a]];
}

/* ---- scraper ---- */
static void snap_path(const Game *g, char *out, const char *ext)
{
    sprintf(out, SNAP_DIR "/%08lX.%s", (unsigned long)g->crc, ext);
}

/* download the screenshot of g if it is not cached yet; returns 1 if a
 * picture file exists afterwards */
/* makes sure the screenshot cache file (snaps/<CRC>.a26i) exists:
 * downloads the PNG if needed and converts it once */
static int fetch_snap(const Game *g)
{
    char path[64], none[64], cache[64], url[512], cmd[900], apath[300];
    FILE *f;
    snap_path(g, cache, "a26i");
    if (exists(cache)) return 1;
    snap_path(g, path, "png");
    if (exists(path)) {
        int ok;
        set(app, MUIA_Application_Sleep, TRUE);
        ok = !snap_convert(path, cache);
        set(app, MUIA_Application_Sleep, FALSE);
        return ok;
    }
    snap_path(g, none, "none");
    if (exists(none) || !g->info || !g->info->name) return 0;
    gamedb_thumb_url(g->info->name, url, sizeof(url));
    abs_path(path, apath, sizeof(apath));
    sprintf(cmd, "curl -s -f -L -o \"%s\" \"%s\"", apath, url);
    set(app, MUIA_Application_Sleep, TRUE);
    run(cmd);
    if (exists(path) && !snap_convert(path, cache)) {
        set(app, MUIA_Application_Sleep, FALSE);
        return 1;
    }
    set(app, MUIA_Application_Sleep, FALSE);
    if (exists(path)) return 0;     /* downloaded but not readable */
    f = fopen(none, "w");           /* not available: do not ask again */
    if (f) fclose(f);
    return 0;
}

static int load_db(void)
{
    int n = gamedb_load(DB_DIR);
    int i;
    for (i = 0; i < ngames; i++) {
        games[i].info = gamedb_find(games[i].crc);
        if (games[i].info && games[i].info->name)
            strncpy(games[i].name, games[i].info->name, sizeof(games[i].name) - 1);
    }
    return n;
}

static void download_db(void)
{
    char cmd[700], path[64], apath[300];
    int i;
    make_dir(DB_DIR);
    set_info("Lade Spieldatenbank (libretro) ...");
    set(app, MUIA_Application_Sleep, TRUE);
    for (i = 0; i < GAMEDB_FILES; i++) {
        sprintf(path, DB_DIR "/%s", gamedb_file[i]);
        abs_path(path, apath, sizeof(apath));
        sprintf(cmd, "curl -s -f -L -o \"%s\" \"%s\"", apath, gamedb_url[i]);
        run(cmd);
    }
    set(app, MUIA_Application_Sleep, FALSE);
    load_db();
    if (ngames) qsort(games, ngames, sizeof(Game), cmp_game);
    fill_list();
}

static void fetch_all_snaps(void)
{
    char msg[160];
    int i;
    for (i = 0; i < ngames; i++) {
        sprintf(msg, "Lade Bilder: %d von %d\n%s", i + 1, ngames, games[i].name);
        set_info(msg);
        DoMethod(app, MUIM_Application_InputBuffered);
        fetch_snap(&games[i]);
    }
    refresh_info();
}

/* ---- info + switches ---- */
static void refresh_info(void)
{
    Game *g = current();
    char buf[800], path[64];
    const char *type;
    trace("select: %s", g ? g->name : "-");
    if (!g) {
        set_info("");
        set(img, MUIA_A26Img_File, NULL);
        return;
    }
    {
        static unsigned char rom[65536];
        FILE *f = fopen(g->path, "rb");
        long n = f ? (long)fread(rom, 1, sizeof(rom), f) : 0;
        if (f) fclose(f);
        type = n ? cart_type_name(cart_detect(rom, (u32)n)) : "?";
    }
    sprintf(buf,
            "\33bName:\33n %s\n\33bHersteller:\33n %s\n\33bEntwickler:\33n %s\n"
            "\33bJahr:\33n %s   \33bGenre:\33n %s\n\33bRegion:\33n %s\n"
            "\33bTyp:\33n %s (%ld KB)   \33bCRC:\33n %08lX",
            g->name,
            g->info && g->info->publisher ? g->info->publisher : "-",
            g->info && g->info->developer ? g->info->developer : "-",
            g->info && g->info->year ? g->info->year : "-",
            g->info && g->info->genre ? g->info->genre : "-",
            g->info && g->info->region ? g->info->region : "-",
            type, g->size / 1024, (unsigned long)g->crc);
    set_info(buf);

    nnset(cy_diff0, MUIA_Cycle_Active, g->diff0);
    nnset(cy_diff1, MUIA_Cycle_Active, g->diff1);
    nnset(cy_tv, MUIA_Cycle_Active, g->bw);
    nnset(cy_region, MUIA_Cycle_Active, g->region);
    nnset(cy_colors, MUIA_Cycle_Active, g->colors);
    nnset(cy_port, MUIA_Cycle_Active, g->port1);

    snap_path(g, path, "a26i");
    if (fetch_snap(g)) set(img, MUIA_A26Img_File, path);
    else set(img, MUIA_A26Img_File, NULL);
}

static void read_switches(void)
{
    Game *g = current();
    ULONG v;
    if (!g) return;
    get(cy_diff0, MUIA_Cycle_Active, &v);  g->diff0 = (UBYTE)v;
    get(cy_diff1, MUIA_Cycle_Active, &v);  g->diff1 = (UBYTE)v;
    get(cy_tv, MUIA_Cycle_Active, &v);     g->bw = (UBYTE)v;
    get(cy_region, MUIA_Cycle_Active, &v); g->region = (UBYTE)v;
    get(cy_colors, MUIA_Cycle_Active, &v); g->colors = (UBYTE)v;
    get(cy_port, MUIA_Cycle_Active, &v);   g->port1 = (UBYTE)v;
    save_switches();
}

static void start_game(void)
{
    Game *g = current();
    char cmd[800], exe[300], rom[300];
    if (!g) return;
    abs_path("PROGDIR:A26", exe, sizeof(exe));
    abs_path(g->path, rom, sizeof(rom));
    sprintf(cmd, "\"%s\" \"%s\"%s%s%s%s%s%s", exe, rom,
            g->diff0 ? " LEFT=A" : "", g->diff1 ? " RIGHT=A" : "", g->bw ? " BW" : "",
            g->region == 1 ? " NTSC" : g->region == 2 ? " PAL" : "",
            g->colors == 1 ? " COLORS=NTSC" : g->colors == 2 ? " COLORS=PAL" : "",
            g->port1 ? " PORT1" : "");
    set(app, MUIA_Application_Sleep, TRUE);
    run(cmd);
    set(app, MUIA_Application_Sleep, FALSE);
}

static void choose_romdir(void)
{
    struct FileRequester *req;
    struct TagItem t[5];
    t[0].ti_Tag = ASLFR_TitleText;     t[0].ti_Data = (ULONG)"ROM-Ordner w\344hlen";
    t[1].ti_Tag = ASLFR_DrawersOnly;   t[1].ti_Data = TRUE;
    t[2].ti_Tag = ASLFR_InitialDrawer; t[2].ti_Data = (ULONG)romdir;
    t[3].ti_Tag = TAG_DONE;
    req = (struct FileRequester *)MUI_AllocAslRequest(ASL_FileRequest, t);
    if (!req) return;
    t[0].ti_Tag = TAG_DONE;
    if (MUI_AslRequest(req, t)) {
        strncpy(romdir, (const char *)req->fr_Drawer, sizeof(romdir) - 1);
        romdir[sizeof(romdir) - 1] = 0;
        save_prefs();
        scan_roms();
        fill_list();
    }
    MUI_FreeAslRequest(req);
}

/* ---------------------------------------------------------------------
 * MUI 3.8 calls gadtools' GetVisualInfoA() with whatever happens to be in
 * a1 as tag list. OS 3.0/3.1 ignored the tags; the gadtools of OS 3.2
 * reads them (GTVI_...) and can run off through memory, which hung the
 * window opening now and then. While A26GUI runs, a small trampoline in
 * front of GetVisualInfoA passes NULL tags for calls from our own task;
 * other tasks go through unchanged.
 */
#define LVO_GetVisualInfoA (-126)

static struct Library *GadToolsBase;
static ULONG *vi_patch;         /* trampoline code + [7] = our task */

static void vi_install(void)
{
    UWORD *c;
    UBYTE *vec;
    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 39);
    if (!GadToolsBase) return;
    vi_patch = (ULONG *)AllocVec(32, MEMF_PUBLIC);
    if (!vi_patch) return;
    c = (UWORD *)vi_patch;
    Forbid();
    vec = (UBYTE *)GadToolsBase + LVO_GetVisualInfoA;
    c[0] = 0x2F0E;                              /* move.l a6,-(sp)      */
    c[1] = 0x2C78; c[2] = 0x0004;               /* movea.l 4.w,a6       */
    c[3] = 0x202E; c[4] = 0x0114;               /* move.l ThisTask(a6),d0 */
    c[5] = 0x2C5F;                              /* movea.l (sp)+,a6     */
    c[6] = 0xB0B9;                              /* cmp.l task.l,d0      */
    *(ULONG *)(c + 7) = (ULONG)&vi_patch[7];
    c[9] = 0x6602;                              /* bne.s +2             */
    c[10] = 0x93C9;                             /* suba.l a1,a1         */
    c[11] = 0x4EF9;                             /* jmp original.l       */
    *(ULONG *)(c + 12) = *(ULONG *)(vec + 2);
    vi_patch[7] = (ULONG)FindTask(NULL);
    CacheClearU();
    SetFunction(GadToolsBase, LVO_GetVisualInfoA, (APTR)vi_patch);
    Permit();
}

static void vi_remove(void)
{
    if (vi_patch) {
        UBYTE *vec = (UBYTE *)GadToolsBase + LVO_GetVisualInfoA;
        Forbid();
        vi_patch[7] = 0;                        /* never matches again */
        /* put the original back unless someone patched after us; the
         * trampoline stays allocated in case a task is inside it */
        if (*(ULONG *)(vec + 2) == (ULONG)vi_patch)
            SetFunction(GadToolsBase, LVO_GetVisualInfoA, (APTR)*(ULONG *)((UWORD *)vi_patch + 12));
        Permit();
    }
    if (GadToolsBase) CloseLibrary(GadToolsBase);
}

/* ---------------------------------------------------------------------
 * screen: checked at the start. RTG Workbench -> window there; PAL/NTSC
 * (chipset) Workbench -> always an own screen in the Workbench's mode with
 * as many planes as the mode allows (Workbench often has only 4 or 8
 * colours; OCS/ECS hires: 16, AGA: 256)
 */
static int force_native;        /* NATIVE: own chipset screen even with RTG */

static struct Screen *open_screen(void)
{
    struct Screen *wb = LockPubScreen(NULL);
    struct Screen *s = NULL;
    ULONG modeid, depth;
    int rtg;
    if (!wb) return NULL;
    rtg = GetBitMapAttr(wb->RastPort.BitMap, BMA_DEPTH) > 8 ||
          !(GetBitMapAttr(wb->RastPort.BitMap, BMA_FLAGS) & BMF_STANDARD);
    modeid = GetVPModeID(&wb->ViewPort);
    if (rtg && force_native) {
        rtg = 0;
        modeid = HIRES_KEY;             /* default monitor (PAL or NTSC) */
    }
    if (!rtg && modeid != INVALID_ID) {
        struct DimensionInfo dims;
        depth = 5;
        if (GetDisplayInfoData(NULL, (UBYTE *)&dims, sizeof(dims), DTAG_DIMS, modeid))
            depth = dims.MaxDepth > 8 ? 8 : dims.MaxDepth;
        {
            static UWORD pens[] = { (UWORD)~0 };
            static struct TagItem t[8];
            t[0].ti_Tag = force_native ? TAG_IGNORE : SA_LikeWorkbench; t[0].ti_Data = TRUE;
            t[1].ti_Tag = SA_DisplayID;     t[1].ti_Data = modeid;
            t[2].ti_Tag = SA_Depth;         t[2].ti_Data = depth;
            t[3].ti_Tag = SA_Title;         t[3].ti_Data = (ULONG)"A26 - Atari 2600";
            t[4].ti_Tag = TAG_IGNORE;
            t[5].ti_Tag = SA_SharePens;     t[5].ti_Data = TRUE;
            t[6].ti_Tag = SA_Pens;          t[6].ti_Data = (ULONG)pens;
            t[7].ti_Tag = TAG_DONE;
            s = OpenScreenTagList(NULL, t);
            {
                char m[40];
                sprintf(m, "%08lx depth %lu %s", modeid, depth, s ? "ok" : "failed");
                trace("screen: %s", m);
            }
            if (s) {
                /* the full-screen backdrop window of build_gui() covers it */
                if (s->Height < 400) ShowTitle(s, FALSE);
            }
        }
    }
    UnlockPubScreen(NULL, wb);
    return s;
}

static const char *const cy_ab[] = { "B", "A", NULL };
static const char *const cy_tvs[] = { "Farbe", "Schwarzwei\337", NULL };
static const char *const cy_reg[] = { "Automatisch", "NTSC", "PAL", NULL };
static const char *const cy_col[] = { "wie Region", "NTSC", "PAL", NULL };
static const char *const cy_p1[] = { "aus", "Mausport-Joystick", NULL };

static Object *label(const char *s)
{
    return MUI_MakeObject(MUIO_Label, s, 0);
}

static Object *cycle(const char *const *entries)
{
    return MUI_NewObject(MUIC_Cycle, MUIA_Cycle_Entries, entries, TAG_DONE);
}

static const char *const reg_pages[] = { "Info", "Konsole", NULL };

static int build_gui(void)
{
    /* small own screen (PAL/NTSC hires, 200..256 lines): full-screen
     * backdrop window, information and switches on two pages */
    int small = own_screen && own_screen->Height < 400;
    Object *konsole, *right;
    img_class = MUI_CreateCustomClass(NULL, MUIC_Area, NULL, sizeof(struct ImgData), (APTR)img_dispatch);
    if (!img_class) return 0;
    img = NewObject(img_class->mcc_Class, NULL, MUIA_Frame, MUIV_Frame_Text,
                    MUIA_Background, MUII_BACKGROUND, TAG_DONE);

    lst_games = MUI_NewObject(MUIC_List, MUIA_Frame, MUIV_Frame_InputList,
                              MUIA_List_ConstructHook, MUIV_List_ConstructHook_String,
                              MUIA_List_DestructHook, MUIV_List_DestructHook_String, TAG_DONE);
    lv_games = MUI_NewObject(MUIC_Listview, MUIA_Listview_List, lst_games, TAG_DONE);
    str_filter = MUI_NewObject(MUIC_String, MUIA_Frame, MUIV_Frame_String, TAG_DONE);
    txt_count = MUI_NewObject(MUIC_Text, MUIA_Text_Contents, "", TAG_DONE);
    /* created with 7 lines so the layout reserves room for the information */
    txt_info = MUI_NewObject(MUIC_Text, MUIA_Frame, MUIV_Frame_Text, MUIA_Background, MUII_TextBack,
                             MUIA_Text_SetVMax, FALSE, MUIA_Text_Contents, "\n\n\n\n\n\n", TAG_DONE);
    cy_diff0 = cycle(cy_ab);
    cy_diff1 = cycle(cy_ab);
    cy_tv = cycle(cy_tvs);
    cy_region = cycle(cy_reg);
    cy_colors = cycle(cy_col);
    cy_port = cycle(cy_p1);
    bt_start = MUI_MakeObject(MUIO_Button, "_Spielen");
    bt_romdir = MUI_MakeObject(MUIO_Button, small ? "_ROMs ..." : "_ROM-Ordner ...");
    bt_db = MUI_MakeObject(MUIO_Button, small ? "_Datenbank" : "_Datenbank laden");
    bt_snaps = MUI_MakeObject(MUIO_Button, small ? "_Bilder" : "Alle _Bilder laden");
    bt_quit = small ? MUI_MakeObject(MUIO_Button, "_Ende") : NULL;

    konsole = MUI_NewObject(MUIC_Group, MUIA_Group_Columns, 2,
        small ? TAG_IGNORE : MUIA_Frame, MUIV_Frame_Group,
        small ? TAG_IGNORE : MUIA_FrameTitle, "Konsole",
        MUIA_Group_Child, label("Difficulty links"),  MUIA_Group_Child, cy_diff0,
        MUIA_Group_Child, label("Difficulty rechts"), MUIA_Group_Child, cy_diff1,
        MUIA_Group_Child, label("TV"),                MUIA_Group_Child, cy_tv,
        MUIA_Group_Child, label("Region"),            MUIA_Group_Child, cy_region,
        MUIA_Group_Child, label("Farbpalette"),       MUIA_Group_Child, cy_colors,
        MUIA_Group_Child, label("Spieler 2"),         MUIA_Group_Child, cy_port,
        TAG_DONE);
    if (small)
        right = MUI_NewObject(MUIC_Group, MUIA_HorizWeight, 100,
            MUIA_Group_Child, img,
            MUIA_Group_Child, MUI_NewObject(MUIC_Register, MUIA_Register_Titles, reg_pages,
                MUIA_Group_Child, txt_info,
                MUIA_Group_Child, konsole,
                TAG_DONE),
            MUIA_Group_Child, MUI_NewObject(MUIC_Group, MUIA_Group_Horiz, TRUE,
                MUIA_Group_Child, bt_start,
                MUIA_Group_Child, bt_quit,
                TAG_DONE),
            TAG_DONE);
    else
        right = MUI_NewObject(MUIC_Group, MUIA_HorizWeight, 100,
            MUIA_Group_Child, img,
            MUIA_Group_Child, txt_info,
            MUIA_Group_Child, konsole,
            MUIA_Group_Child, bt_start,
            TAG_DONE);

    win = MUI_NewObject(MUIC_Window,
        MUIA_Window_Title, small ? NULL : "A26 - Atari 2600 Emulator",
        /* the remembered size belongs to the Workbench window */
        own_screen ? TAG_IGNORE : MUIA_Window_ID, MAKE_ID('A','2','6','G'),
        own_screen ? MUIA_Window_Screen : TAG_IGNORE, own_screen,
        small ? MUIA_Window_Backdrop : TAG_IGNORE, TRUE,
        small ? MUIA_Window_Borderless : TAG_IGNORE, TRUE,
        small ? MUIA_Window_CloseGadget : TAG_IGNORE, FALSE,
        small ? MUIA_Window_DepthGadget : TAG_IGNORE, FALSE,
        small ? MUIA_Window_SizeGadget : TAG_IGNORE, FALSE,
        small ? MUIA_Window_DragBar : TAG_IGNORE, FALSE,
        small ? MUIA_Window_LeftEdge : TAG_IGNORE, 0,
        small ? MUIA_Window_TopEdge : TAG_IGNORE, 0,
        small ? MUIA_Window_Width : TAG_IGNORE, MUIV_Window_Width_Screen(100),
        small ? MUIA_Window_Height : TAG_IGNORE, MUIV_Window_Height_Screen(100),
        MUIA_Window_RootObject, MUI_NewObject(MUIC_Group, MUIA_Group_Horiz, TRUE,
            MUIA_Group_Child, MUI_NewObject(MUIC_Group, MUIA_HorizWeight, 120,
                MUIA_Group_Child, MUI_NewObject(MUIC_Group, MUIA_Group_Horiz, TRUE,
                    MUIA_Group_Child, label("Filter"),
                    MUIA_Group_Child, str_filter,
                    MUIA_Group_Child, txt_count,
                    TAG_DONE),
                MUIA_Group_Child, lv_games,
                MUIA_Group_Child, MUI_NewObject(MUIC_Group, MUIA_Group_Horiz, TRUE,
                    MUIA_Group_Child, bt_romdir,
                    MUIA_Group_Child, bt_db,
                    MUIA_Group_Child, bt_snaps,
                    TAG_DONE),
                TAG_DONE),
            MUIA_Group_Child, right,
            TAG_DONE),
        TAG_DONE);

    app = MUI_NewObject(MUIC_Application,
        MUIA_Application_Title, "A26GUI",
        MUIA_Application_Version, vers,
        MUIA_Application_Copyright, "AmiAtari2600",
        MUIA_Application_Author, "AmiAtari2600",
        MUIA_Application_Description, "Atari 2600 Emulator",
        MUIA_Application_Base, "A26GUI",
        MUIA_Application_Window, win,
        TAG_DONE);
    trace(app ? "application ok" : "application failed", NULL);
    if (!app) return 0;

    DoMethod(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, app, 2,
             MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    if (bt_quit)
        DoMethod(bt_quit, MUIM_Notify, MUIA_Pressed, FALSE, app, 2,
                 MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    DoMethod(lst_games, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime, app, 2,
             MUIM_Application_ReturnID, ID_SELECT);
    DoMethod(lv_games, MUIM_Notify, MUIA_Listview_DoubleClick, TRUE, app, 2,
             MUIM_Application_ReturnID, ID_START);
    DoMethod(bt_start, MUIM_Notify, MUIA_Pressed, FALSE, app, 2,
             MUIM_Application_ReturnID, ID_START);
    DoMethod(str_filter, MUIM_Notify, MUIA_String_Contents, MUIV_EveryTime, app, 2,
             MUIM_Application_ReturnID, ID_FILTER);
    DoMethod(bt_romdir, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_ROMDIR);
    DoMethod(bt_db, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_DB);
    DoMethod(bt_snaps, MUIM_Notify, MUIA_Pressed, FALSE, app, 2, MUIM_Application_ReturnID, ID_SNAPS);
    DoMethod(cy_diff0, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, app, 2, MUIM_Application_ReturnID, ID_SWITCH);
    DoMethod(cy_diff1, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, app, 2, MUIM_Application_ReturnID, ID_SWITCH);
    DoMethod(cy_tv, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, app, 2, MUIM_Application_ReturnID, ID_SWITCH);
    DoMethod(cy_region, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, app, 2, MUIM_Application_ReturnID, ID_SWITCH);
    DoMethod(cy_colors, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, app, 2, MUIM_Application_ReturnID, ID_SWITCH);
    DoMethod(cy_port, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, app, 2, MUIM_Application_ReturnID, ID_SWITCH);
    set(win, MUIA_Window_ActiveObject, lv_games);
    return 1;
}

int main(int argc, char **argv)
{
    ULONG sigs = 0;
    LONG id;
    int i;

    (void)vers;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    DataTypesBase = OpenLibrary((CONST_STRPTR)"datatypes.library", 39);
    UtilityBase = OpenLibrary((CONST_STRPTR)"utility.library", 39);
    MUIMasterBase = OpenLibrary((CONST_STRPTR)MUIMASTER_NAME, 19);
    if (!IntuitionBase || !GfxBase || !DataTypesBase || !UtilityBase || !MUIMasterBase) {
        printf("A26GUI needs OS 3.0+ and MUI 3.8+\n");
        goto out;
    }
    for (i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "ROMDIR=", 7) || !strncmp(argv[i], "romdir=", 7))
            strncpy(romdir, argv[i] + 7, sizeof(romdir) - 1);
        else if (!strcmp(argv[i], "DEBUG") || !strcmp(argv[i], "debug"))
        {
            FILE *f = fopen("PROGDIR:A26GUI.log", "w");
            if (f) fclose(f);
            debug = 1;
        }
        else if (!strcmp(argv[i], "NATIVE") || !strcmp(argv[i], "native"))
            force_native = 1;
    }
    trace("start %s", VERSION);
    if (GetProgramDir()) NameFromLock(GetProgramDir(), (STRPTR)progdir, sizeof(progdir));
    trace("progdir %s", progdir);
    load_prefs();
    make_dir(SNAP_DIR);
    trace("database", NULL);
    load_db();
    trace("scan %s", romdir);
    scan_roms();

    vi_install();
    CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 40);
    own_screen = open_screen();
    trace("build gui", NULL);
    if (!build_gui()) {
        printf("cannot create the MUI application\n");
        goto out;
    }
    trace("open window", NULL);
    set(win, MUIA_Window_Open, TRUE);
    {
        ULONG open = FALSE;
        get(win, MUIA_Window_Open, &open);
        trace(open ? "window open" : "window failed", NULL);
        if (!open) {
            printf("cannot open the window (screen too small?)\n");
            goto out;
        }
    }
    fill_list();
    if (!exists(DB_DIR "/nointro.dat")) download_db();
    trace("ready", NULL);

    while ((id = (LONG)DoMethod(app, MUIM_Application_NewInput, &sigs)) !=
           (LONG)MUIV_Application_ReturnID_Quit) {
        switch (id) {
        case ID_SELECT: refresh_info(); break;
        case ID_START:  start_game(); break;
        case ID_FILTER: fill_list(); break;
        case ID_SWITCH: read_switches(); break;
        case ID_ROMDIR: choose_romdir(); break;
        case ID_DB:     download_db(); break;
        case ID_SNAPS:  fetch_all_snaps(); break;
        }
        if (sigs) {
            sigs = Wait(sigs | SIGBREAKF_CTRL_C);
            if (sigs & SIGBREAKF_CTRL_C) break;
        } else if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
            break;
        }
    }

out:
    if (app) MUI_DisposeObject(app);
    if (img_class) MUI_DeleteCustomClass(img_class);
    if (own_screen) CloseScreen(own_screen);
    gamedb_free();
    free(games);
    free(shown);
    trace("exit", NULL);
    vi_remove();
    if (CyberGfxBase) CloseLibrary(CyberGfxBase);
    if (MUIMasterBase) CloseLibrary(MUIMasterBase);
    if (UtilityBase) CloseLibrary(UtilityBase);
    if (DataTypesBase) CloseLibrary(DataTypesBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
