/*
 * vidconv.c - TIA line -> Amiga bitplanes + per-line palette.
 * See vidconv.h for the idea.
 */
#include "vidconv.h"
#include "../core/palette.h"

u32 vc_stat_fallbacks;
u32 vc_stat_moves;

static u16 rgb12[128];          /* TIA colour index -> $0RGB */
static u8  slot_of[128];        /* TIA colour index -> register, 0xFF = not loaded */
static u8  color_in[32];        /* register -> TIA colour index, 0xFF = free */
static u16 last_used[32];       /* line stamp for LRU */
static u16 line_stamp;

/* c2p tables: planes 0-3 in the bytes of a longword, plane 4 separately */
static u32 c2p_lo[32];
static u8  c2p_hi[32];
static int c2p_ready;

static void c2p_init(void)
{
    int s;
    for (s = 0; s < 32; s++) {
        c2p_lo[s] = ((s & 1) ? 0xC0000000UL : 0) | ((s & 2) ? 0x00C00000UL : 0) |
                    ((s & 4) ? 0x0000C000UL : 0) | ((s & 8) ? 0x000000C0UL : 0);
        c2p_hi[s] = (u8)((s & 16) ? 0xC0 : 0);
    }
    c2p_ready = 1;
}

void vc_set_palette(const u32 *palette24)
{
    int i;
    for (i = 0; i < 128; i++) rgb12[i] = palette_to_rgb12(palette24[i]);
    if (!c2p_ready) c2p_init();
    vc_begin_frame();
}

void vc_begin_frame(void)
{
    int i;
    for (i = 0; i < 128; i++) slot_of[i] = 0xFF;
    for (i = 0; i < 32; i++) { color_in[i] = 0xFF; last_used[i] = 0; }
    line_stamp = 1;
    vc_stat_fallbacks = 0;
    vc_stat_moves = 0;
}

static int rgb_dist(u16 a, u16 b)
{
    int dr = (int)((a >> 8) & 15) - (int)((b >> 8) & 15);
    int dg = (int)((a >> 4) & 15) - (int)((b >> 4) & 15);
    int db = (int)(a & 15) - (int)(b & 15);
    return dr * dr * 3 + dg * dg * 4 + db * db * 2;
}

/* nearest register among those already used by this line */
static u8 nearest(u8 c, u32 used)
{
    int s, best = 0, bd = 0x7FFFFFFF;
    for (s = 0; s < 32; s++) {
        if ((used & (1UL << s)) && color_in[s] != 0xFF) {
            int d = rgb_dist(rgb12[c], rgb12[color_in[s]]);
            if (d < bd) { bd = d; best = s; }
        }
    }
    return (u8)best;
}

int vc_map_line(const u8 *tia_line, u8 *slots, VcMove *moves)
{
    u32 used = 0;
    int nmoves = 0, x;
    u8 prev_c = 0xFF, prev_s = 0;

    for (x = 0; x < VC_WIDTH; x++) {
        u8 c = (u8)(tia_line[x] >> 1);
        u8 s;
        if (c == prev_c) { slots[x] = prev_s; continue; }
        s = slot_of[c];
        if (s == 0xFF) {
            /* load the colour into the least recently used free register */
            int best = -1, r;
            if (nmoves < VC_MAX_MOVES) {
                u16 oldest = 0xFFFF;
                for (r = 0; r < 32; r++) {
                    if (used & (1UL << r)) continue;
                    if (color_in[r] == 0xFF) { best = r; break; }
                    if (last_used[r] < oldest) { oldest = last_used[r]; best = r; }
                }
            }
            if (best >= 0) {
                if (color_in[best] != 0xFF) slot_of[color_in[best]] = 0xFF;
                color_in[best] = c;
                slot_of[c] = (u8)best;
                moves[nmoves].reg = (u16)best;
                moves[nmoves].rgb = rgb12[c];
                nmoves++;
                s = (u8)best;
            } else {
                s = nearest(c, used);
                vc_stat_fallbacks++;
                /* not cached: the substitute must not become the colour's slot */
                slots[x] = s;
                prev_c = 0xFF;
                used |= 1UL << s;
                last_used[s] = line_stamp;
                continue;
            }
        }
        used |= 1UL << s;
        last_used[s] = line_stamp;
        slots[x] = s;
        prev_c = c;
        prev_s = s;
    }
    line_stamp++;
    vc_stat_moves += (u32)nmoves;
    return nmoves;
}

#ifdef A26_BIG_ENDIAN
#define PUT32(p, v) (*(u32 *)(void *)(p) = (v))
#else
#define PUT32(p, v) do { u8 *q_ = (p); u32 v_ = (v); \
        q_[0] = (u8)(v_ >> 24); q_[1] = (u8)(v_ >> 16); q_[2] = (u8)(v_ >> 8); q_[3] = (u8)v_; } while (0)
#endif

void vc_c2p_line(const u8 *s, u8 *dst)
{
    int g;
    /* 16 TIA pixels = 32 lowres pixels = one longword per plane */
    for (g = 0; g < VC_WIDTH / 16; g++) {
        u32 a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0;
        int k;
        for (k = 0; k < 4; k++) {
            u32 v = c2p_lo[s[0]] | (c2p_lo[s[1]] >> 2) | (c2p_lo[s[2]] >> 4) | (c2p_lo[s[3]] >> 6);
            u32 w = (u32)(c2p_hi[s[0]] | (c2p_hi[s[1]] >> 2) | (c2p_hi[s[2]] >> 4) | (c2p_hi[s[3]] >> 6));
            a0 = (a0 << 8) | (v >> 24);
            a1 = (a1 << 8) | ((v >> 16) & 0xFF);
            a2 = (a2 << 8) | ((v >> 8) & 0xFF);
            a3 = (a3 << 8) | (v & 0xFF);
            a4 = (a4 << 8) | w;
            s += 4;
        }
        PUT32(dst + 0 * VC_ROW_BYTES + g * 4, a0);
        PUT32(dst + 1 * VC_ROW_BYTES + g * 4, a1);
        PUT32(dst + 2 * VC_ROW_BYTES + g * 4, a2);
        PUT32(dst + 3 * VC_ROW_BYTES + g * 4, a3);
        PUT32(dst + 4 * VC_ROW_BYTES + g * 4, a4);
    }
}
