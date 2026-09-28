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
#include "../core/cart.h"

#define VERSION "0.1"
static const char vers[] = "$VER: A26GUI " VERSION " (" __DATE__ ")";

struct Library *MUIMasterBase;
struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *DataTypesBase;
struct Library *UtilityBase;

/* ---- trace log (PROGDIR:A26GUI.log), flushed line by line so it
 * survives a crash; enabled with DEBUG ---- */
static FILE *logf;
static void trace(const char *fmt, const char *arg)
{
    if (!logf) return;
    fprintf(logf, fmt, arg ? arg : "");
    fputc('\n', logf);
    fflush(logf);
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
static Object *bt_start, *bt_romdir, *bt_db, *bt_snaps;
static struct Screen *own_screen;

enum { ID_SELECT = 1, ID_START, ID_FILTER, ID_SWITCH, ID_ROMDIR, ID_DB, ID_SNAPS };

/* ---------------------------------------------------------------------
 * screenshot area: a small custom class drawing a datatype picture
 * (remapped to the screen) in a 320 x 240 area. The libretro snaps come in
 * different sizes (320 x 210..250, 512 x 384) with uneven borders, so the
 * border (the colour of the top left pixel) is cut off and the rest is
 * scaled to fit, keeping its aspect, and centred.
 */
#define MUIA_A26Img_File (TAG_USER | 0x26000001)

struct ImgData {
    Object *dto;
    struct BitMap *bm;
    LONG w, h;
    LONG cx, cy, cw, ch;        /* content without border */
    struct BitMap *sbm;         /* scaled content */
    LONG sw, sh, aw, ah;        /* its size, for an area of aw x ah */
    int setup;
    char file[256];
};

static struct MUI_CustomClass *img_class;

static void img_free(struct ImgData *d)
{
    if (d->sbm) { WaitBlit(); FreeBitMap(d->sbm); }
    d->sbm = NULL;
    if (d->dto) DisposeDTObject(d->dto);
    d->dto = NULL;
    d->bm = NULL;
}

/* is the pixel (r,g,b) different from the border colour? */
static int differs(const UBYTE *p, const UBYTE *b)
{
    int dr = p[0] - b[0], dg = p[1] - b[1], db = p[2] - b[2];
    return dr * dr + dg * dg + db * db > 3 * 24 * 24;
}

/* bounding box of everything that is not border; whole picture if the
 * datatype cannot deliver RGB pixels */
static void img_bounds(struct ImgData *d)
{
    UBYTE *row, border[3];
    LONG x, y, x0 = d->w, x1 = -1, y0 = d->h, y1 = -1;
    d->cx = d->cy = 0; d->cw = d->w; d->ch = d->h;
    if (d->w <= 0 || d->h <= 0) return;
    row = (UBYTE *)AllocVec(d->w * 3, MEMF_ANY);
    if (!row) return;
    for (y = 0; y < d->h; y++) {
        if (!DoMethod(d->dto, PDTM_READPIXELARRAY, (ULONG)row, PBPAFMT_RGB,
                      d->w * 3, 0, y, d->w, 1))
            break;
        if (y == 0) { border[0] = row[0]; border[1] = row[1]; border[2] = row[2]; }
        for (x = 0; x < d->w; x++)
            if (differs(row + x * 3, border)) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                y1 = y;
            }
    }
    FreeVec(row);
    if (y < d->h || x1 < x0) return;     /* no pixels or all border */
    d->cx = x0; d->cy = y0; d->cw = x1 - x0 + 1; d->ch = y1 - y0 + 1;
    {
        char t[48];
        sprintf(t, "%ld,%ld %ldx%ld", d->cx, d->cy, d->cw, d->ch);
        trace("image: content %s", t);
    }
}

/* content scaled to fit aw x ah */
static void img_scale(struct ImgData *d, LONG aw, LONG ah)
{
    struct BitScaleArgs bsa;
    LONG dw, dh;
    if (d->sbm && d->aw == aw && d->ah == ah) return;
    if (d->sbm) { WaitBlit(); FreeBitMap(d->sbm); d->sbm = NULL; }
    d->aw = aw; d->ah = ah;
    if (d->cw * ah <= d->ch * aw) { dh = ah; dw = d->cw * ah / d->ch; }
    else { dw = aw; dh = d->ch * aw / d->cw; }
    if (dw < 1 || dh < 1) return;
    d->sbm = AllocBitMap(dw, dh, GetBitMapAttr(d->bm, BMA_DEPTH), BMF_CLEAR, d->bm);
    if (!d->sbm) return;
    memset(&bsa, 0, sizeof(bsa));
    bsa.bsa_SrcX = d->cx;        bsa.bsa_SrcY = d->cy;
    bsa.bsa_SrcWidth = d->cw;    bsa.bsa_SrcHeight = d->ch;
    bsa.bsa_XSrcFactor = d->cw;  bsa.bsa_YSrcFactor = d->ch;
    bsa.bsa_XDestFactor = dw;    bsa.bsa_YDestFactor = dh;
    bsa.bsa_SrcBitMap = d->bm;   bsa.bsa_DestBitMap = d->sbm;
    BitMapScale(&bsa);
    d->sw = bsa.bsa_DestWidth;   d->sh = bsa.bsa_DestHeight;
    {
        char t[24];
        sprintf(t, "%ldx%ld", d->sw, d->sh);
        trace("image: scaled %s", t);
    }
}

static void img_load(Object *obj, struct ImgData *d)
{
    struct TagItem tags[7];
    img_free(d);
    if (!d->setup || !d->file[0]) return;
    tags[0].ti_Tag = DTA_GroupID;       tags[0].ti_Data = GID_PICTURE;
    tags[1].ti_Tag = PDTA_Remap;        tags[1].ti_Data = TRUE;
    tags[2].ti_Tag = PDTA_Screen;       tags[2].ti_Data = (ULONG)_screen(obj);
    tags[3].ti_Tag = PDTA_DestMode;     tags[3].ti_Data = PMODE_V43;
    tags[4].ti_Tag = PDTA_UseFriendBitMap; tags[4].ti_Data = TRUE;
    tags[5].ti_Tag = OBP_Precision;     tags[5].ti_Data = PRECISION_IMAGE;
    tags[6].ti_Tag = TAG_DONE;
    trace("image: load %s", d->file);
    d->dto = NewDTObjectA((APTR)d->file, tags);
    trace("image: object %s", d->dto ? "ok" : "failed");
    if (!d->dto) return;
    if (DoMethod(d->dto, DTM_PROCLAYOUT, NULL, 1)) {
        struct BitMapHeader *bmh = NULL;
        struct TagItem gt[3];
        gt[0].ti_Tag = PDTA_DestBitMap;  gt[0].ti_Data = (ULONG)&d->bm;
        gt[1].ti_Tag = PDTA_BitMapHeader; gt[1].ti_Data = (ULONG)&bmh;
        gt[2].ti_Tag = TAG_DONE;
        GetDTAttrsA(d->dto, gt);
        if (bmh) { d->w = bmh->bmh_Width; d->h = bmh->bmh_Height; }
        else d->bm = NULL;
    }
    trace("image: bitmap %s", d->bm ? "ok" : "none");
    if (!d->bm) { img_free(d); return; }
    img_bounds(d);
}

static void img_set(Object *obj, struct ImgData *d, struct TagItem *tags)
{
    struct TagItem *t = FindTagItem(MUIA_A26Img_File, tags);
    if (!t) return;
    if (t->ti_Data) {
        strncpy(d->file, (const char *)t->ti_Data, sizeof(d->file) - 1);
        d->file[sizeof(d->file) - 1] = 0;
    } else {
        d->file[0] = 0;
    }
    img_load(obj, d);
}

static ULONG img_dispatch(__reg("a0") struct IClass *cl, __reg("a2") Object *obj,
                          __reg("a1") Msg msg)
{
    struct ImgData *d;
    switch (msg->MethodID) {
    case OM_NEW:
        obj = (Object *)DoSuperMethodA(cl, obj, msg);
        if (obj) {
            d = INST_DATA(cl, obj);
            memset(d, 0, sizeof(*d));
            img_set(obj, d, ((struct opSet *)msg)->ops_AttrList);
        }
        return (ULONG)obj;
    case OM_DISPOSE:
        img_free(INST_DATA(cl, obj));
        break;
    case OM_SET:
        d = INST_DATA(cl, obj);
        img_set(obj, d, ((struct opSet *)msg)->ops_AttrList);
        MUI_Redraw(obj, MADF_DRAWOBJECT);
        break;
    case MUIM_Setup:
        if (!DoSuperMethodA(cl, obj, msg)) return FALSE;
        d = INST_DATA(cl, obj);
        d->setup = 1;
        img_load(obj, d);
        return TRUE;
    case MUIM_Cleanup:
        d = INST_DATA(cl, obj);
        img_free(d);
        d->setup = 0;
        break;
    case MUIM_AskMinMax: {
        struct MUI_MinMax *mm;
        DoSuperMethodA(cl, obj, msg);
        mm = ((struct MUIP_AskMinMax *)msg)->MinMaxInfo;
        mm->MinWidth += 320; mm->DefWidth += 320; mm->MaxWidth += 320;
        mm->MinHeight += 240; mm->DefHeight += 240; mm->MaxHeight += 240;
        return 0;
    }
    case MUIM_Draw:
        DoSuperMethodA(cl, obj, msg);
        d = INST_DATA(cl, obj);
        if ((((struct MUIP_Draw *)msg)->flags & MADF_DRAWOBJECT)) {
            LONG x = _mleft(obj), y = _mtop(obj), w = _mwidth(obj), h = _mheight(obj);
            SetAPen(_rp(obj), _dri(obj)->dri_Pens[BACKGROUNDPEN]);
            RectFill(_rp(obj), x, y, x + w - 1, y + h - 1);
            if (d->bm) img_scale(d, w, h);
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
static int fetch_snap(const Game *g)
{
    char path[64], none[64], url[512], cmd[900], apath[300];
    FILE *f;
    snap_path(g, path, "png");
    if (exists(path)) return 1;
    snap_path(g, none, "none");
    if (exists(none) || !g->info || !g->info->name) return 0;
    gamedb_thumb_url(g->info->name, url, sizeof(url));
    abs_path(path, apath, sizeof(apath));
    sprintf(cmd, "curl -s -f -L -o \"%s\" \"%s\"", apath, url);
    set(app, MUIA_Application_Sleep, TRUE);
    run(cmd);
    set(app, MUIA_Application_Sleep, FALSE);
    if (exists(path)) return 1;
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

    snap_path(g, path, "png");
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
 * screen: RTG Workbench -> window there; native -> own screen with more
 * colours (same mode as the Workbench, as many planes as it allows)
 */
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
    if (!rtg && modeid != INVALID_ID) {
        struct DimensionInfo dims;
        depth = 5;
        if (GetDisplayInfoData(NULL, (UBYTE *)&dims, sizeof(dims), DTAG_DIMS, modeid))
            depth = dims.MaxDepth > 8 ? 8 : dims.MaxDepth;
        if (depth > wb->RastPort.BitMap->Depth) {
            struct TagItem t[7];
            t[0].ti_Tag = SA_LikeWorkbench; t[0].ti_Data = TRUE;
            t[1].ti_Tag = SA_DisplayID;     t[1].ti_Data = modeid;
            t[2].ti_Tag = SA_Depth;         t[2].ti_Data = depth;
            t[3].ti_Tag = SA_Title;         t[3].ti_Data = (ULONG)"A26 - Atari 2600";
            t[4].ti_Tag = SA_PubName;       t[4].ti_Data = (ULONG)"A26GUI";
            t[5].ti_Tag = SA_SharePens;     t[5].ti_Data = TRUE;
            t[6].ti_Tag = TAG_DONE;
            s = OpenScreenTagList(NULL, t);
            if (s) PubScreenStatus(s, 0);
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

static int build_gui(void)
{
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
    bt_romdir = MUI_MakeObject(MUIO_Button, "_ROM-Ordner ...");
    bt_db = MUI_MakeObject(MUIO_Button, "_Datenbank laden");
    bt_snaps = MUI_MakeObject(MUIO_Button, "Alle _Bilder laden");

    win = MUI_NewObject(MUIC_Window,
        MUIA_Window_Title, "A26 - Atari 2600 Emulator",
        MUIA_Window_ID, MAKE_ID('A','2','6','G'),
        own_screen ? MUIA_Window_Screen : TAG_IGNORE, own_screen,
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
            MUIA_Group_Child, MUI_NewObject(MUIC_Group, MUIA_HorizWeight, 100,
                MUIA_Group_Child, img,
                MUIA_Group_Child, txt_info,
                MUIA_Group_Child, MUI_NewObject(MUIC_Group, MUIA_Group_Columns, 2,
                    MUIA_Frame, MUIV_Frame_Group, MUIA_FrameTitle, "Konsole",
                    MUIA_Group_Child, label("Difficulty links"),  MUIA_Group_Child, cy_diff0,
                    MUIA_Group_Child, label("Difficulty rechts"), MUIA_Group_Child, cy_diff1,
                    MUIA_Group_Child, label("TV"),                MUIA_Group_Child, cy_tv,
                    MUIA_Group_Child, label("Region"),            MUIA_Group_Child, cy_region,
                    MUIA_Group_Child, label("Farbpalette"),       MUIA_Group_Child, cy_colors,
                    MUIA_Group_Child, label("Spieler 2"),         MUIA_Group_Child, cy_port,
                    TAG_DONE),
                MUIA_Group_Child, bt_start,
                TAG_DONE),
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
    if (!app) return 0;

    DoMethod(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, app, 2,
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
            logf = fopen("PROGDIR:A26GUI.log", "w");
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

    own_screen = open_screen();
    if (!build_gui()) {
        printf("cannot create the MUI application\n");
        goto out;
    }
    set(win, MUIA_Window_Open, TRUE);
    trace("window open", NULL);
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
    if (logf) fclose(logf);
    if (MUIMasterBase) CloseLibrary(MUIMasterBase);
    if (UtilityBase) CloseLibrary(UtilityBase);
    if (DataTypesBase) CloseLibrary(DataTypesBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
