/*
 * cart.c - cartridge ROM mapping and bankswitching.
 *
 * Most schemes are handled with four 1K read segment pointers covering
 * $1000-$1FFF, so the common read path is a single table lookup. Only
 * accesses to hotspot/RAM areas take the slow path.
 *
 * The CPU passes full 16-bit addresses; the FE (Activision) scheme uses
 * A13 of the CPU address (code at $Dxxx vs. $Fxxx), exactly as the real
 * cartridge effectively does by watching the JSR/RTS stack traffic.
 */
#include <string.h>
#include "cart.h"

Cart cart;
int  cart_tia_hook;

static u8 small_rom[4096];      /* mirrored image for ROMs < 4K */

static const char *const type_names[CART_TYPE_COUNT] = {
    "unknown", "2K", "4K", "F8", "F6", "F4", "F8SC", "F6SC", "F4SC",
    "FA", "E0", "E7", "3F", "FE"
};

const char *cart_type_name(CartType t)
{
    return (t < CART_TYPE_COUNT) ? type_names[t] : "?";
}

CartType cart_type_from_name(const char *name)
{
    int i;
    for (i = 1; i < CART_TYPE_COUNT; i++) {
        const char *a = type_names[i], *b = name;
        while (*a && *b && (*a == *b || (*a ^ 0x20) == *b)) { a++; b++; }
        if (!*a && !*b) return (CartType)i;
    }
    return CART_UNKNOWN;
}

/* ---- detection ------------------------------------------------------- */

static int search(const u8 *rom, u32 size, const u8 *sig, u32 len, int min_count)
{
    u32 i, j;
    int count = 0;
    if (size < len) return 0;
    for (i = 0; i <= size - len; i++) {
        for (j = 0; j < len && rom[i + j] == sig[j]; j++)
            ;
        if (j == len && ++count >= min_count) return 1;
    }
    return 0;
}

static int any_sig(const u8 *rom, u32 size, const u8 sigs[][5], int n, u32 len)
{
    int i;
    for (i = 0; i < n; i++)
        if (search(rom, size, sigs[i], len, 1)) return 1;
    return 0;
}

/* Superchip carts carry a dummy RAM area: the first 128 bytes of each 4K
 * bank are repeated in the next 128 bytes. */
static int is_superchip(const u8 *rom, u32 size)
{
    u32 b;
    for (b = 0; b + 4096 <= size; b += 4096)
        if (memcmp(rom + b, rom + b + 128, 128) != 0) return 0;
    return 1;
}

static int is_e0(const u8 *rom, u32 size)
{
    static const u8 sigs[][5] = {
        { 0x8D, 0xE0, 0x1F }, { 0x8D, 0xE0, 0x5F }, { 0x8D, 0xE9, 0xFF },
        { 0x0C, 0xE0, 0x1F }, { 0xAD, 0xE0, 0x1F }, { 0xAD, 0xE9, 0xFF },
        { 0xAD, 0xED, 0xFF }, { 0xAD, 0xF3, 0xBF }
    };
    return any_sig(rom, size, sigs, 8, 3);
}

static int is_e7(const u8 *rom, u32 size)
{
    static const u8 sigs[][5] = {
        { 0xAD, 0xE2, 0xFF }, { 0xAD, 0xE5, 0xFF }, { 0xAD, 0xE5, 0x1F },
        { 0xAD, 0xE7, 0x1F }, { 0x0C, 0xE7, 0x1F }, { 0x8D, 0xE7, 0xFF },
        { 0x8D, 0xE7, 0x1F }
    };
    return any_sig(rom, size, sigs, 7, 3);
}

static int is_3f(const u8 *rom, u32 size)
{
    static const u8 sig[2] = { 0x85, 0x3F };   /* STA $3F */
    return search(rom, size, sig, 2, 2);
}

static int is_fe(const u8 *rom, u32 size)
{
    static const u8 sigs[][5] = {
        { 0x20, 0x00, 0xD0, 0xC6, 0xC5 }, { 0x20, 0xC3, 0xF8, 0xA5, 0x82 },
        { 0xD0, 0xFB, 0x20, 0x73, 0xFE }, { 0x20, 0x00, 0xF0, 0x84, 0xD6 }
    };
    return any_sig(rom, size, sigs, 4, 5);
}

CartType cart_detect(const u8 *rom, u32 size)
{
    if (size <= 2048) return CART_2K;
    if (size <= 4096) return CART_4K;
    if (size == 8192) {
        if (is_superchip(rom, size)) return CART_F8SC;
        if (is_e0(rom, size)) return CART_E0;
        if (is_3f(rom, size)) return CART_3F;
        if (is_fe(rom, size)) return CART_FE;
        return CART_F8;
    }
    if (size == 12288) return CART_FA;
    if (size == 16384) {
        if (is_superchip(rom, size)) return CART_F6SC;
        if (is_e7(rom, size)) return CART_E7;
        if (is_3f(rom, size)) return CART_3F;
        return CART_F6;
    }
    if (size == 32768) {
        if (is_superchip(rom, size)) return CART_F4SC;
        if (is_3f(rom, size)) return CART_3F;
        return CART_F4;
    }
    if ((size % 2048) == 0 && is_3f(rom, size)) return CART_3F;
    return CART_UNKNOWN;
}

/* ---- mapping helpers ------------------------------------------------- */

static void map_4k(u32 offset)
{
    int i;
    for (i = 0; i < 4; i++) cart.seg[i] = cart.rom + offset + (u32)i * 1024;
}

static u32 nbanks_4k(void) { return cart.size / 4096; }

static void select_bank(u8 bank)
{
    cart.bank = bank;
    map_4k((u32)bank * 4096);
}

static void e7_map(void)
{
    /* $1000-$17FF: ROM slice 0-6 (slice 7 = RAM, handled in read) */
    if (cart.e7_rom_bank < 7) {
        cart.seg[0] = cart.rom + (u32)cart.e7_rom_bank * 2048;
        cart.seg[1] = cart.seg[0] + 1024;
    }
    /* $1800-$1FFF: last slice (the first 512 bytes are RAM, see read) */
    cart.seg[2] = cart.rom + 7 * 2048;
    cart.seg[3] = cart.seg[2] + 1024;
}

static void threef_map(void)
{
    u32 banks = cart.size / 2048;
    cart.seg[0] = cart.rom + (u32)(cart.bank % banks) * 2048;
    cart.seg[1] = cart.seg[0] + 1024;
    cart.seg[2] = cart.rom + cart.size - 2048;
    cart.seg[3] = cart.seg[2] + 1024;
}

int cart_init(u8 *rom, u32 size, CartType type)
{
    if (!rom || size == 0) return -1;
    if (type == CART_UNKNOWN) type = cart_detect(rom, size);
    if (type == CART_UNKNOWN) return -2;

    memset(&cart, 0, sizeof(cart));
    cart.type = type;
    cart.rom = rom;
    cart.size = size;

    if (size < 4096) {
        u32 i;
        for (i = 0; i < 4096; i++) small_rom[i] = rom[i % size];
        cart.rom = small_rom;
        cart.size = 4096;
        cart.type = CART_4K;
    }

    switch (cart.type) {
    case CART_F8: case CART_F8SC: if (cart.size != 8192) return -3; break;
    case CART_F6: case CART_F6SC: case CART_E7: if (cart.size != 16384) return -3; break;
    case CART_F4: case CART_F4SC: if (cart.size != 32768) return -3; break;
    case CART_FA: if (cart.size != 12288) return -3; break;
    case CART_E0: case CART_FE: if (cart.size != 8192) return -3; break;
    case CART_3F: if (cart.size % 2048) return -3; break;
    default: break;
    }
    cart_tia_hook = (cart.type == CART_3F);
    cart_reset();
    return 0;
}

void cart_reset(void)
{
    switch (cart.type) {
    case CART_E0:
        cart.seg[0] = cart.rom + 4 * 1024;
        cart.seg[1] = cart.rom + 5 * 1024;
        cart.seg[2] = cart.rom + 6 * 1024;
        cart.seg[3] = cart.rom + 7 * 1024;
        break;
    case CART_E7:
        cart.e7_rom_bank = 0;
        cart.e7_ram_bank = 0;
        e7_map();
        break;
    case CART_3F:
        cart.bank = 0;
        threef_map();
        break;
    case CART_FE:
        map_4k(0);
        break;
    case CART_4K:
        map_4k(0);
        break;
    default:
        /* start in the last bank: it always holds a valid reset vector */
        select_bank((u8)(nbanks_4k() - 1));
        break;
    }
}

/* ---- access ---------------------------------------------------------- */

static void hotspot(u16 a)
{
    switch (cart.type) {
    case CART_F8: case CART_F8SC:
        if (a == 0xFF8 || a == 0xFF9) select_bank((u8)(a - 0xFF8));
        break;
    case CART_F6: case CART_F6SC:
        if (a >= 0xFF6 && a <= 0xFF9) select_bank((u8)(a - 0xFF6));
        break;
    case CART_F4: case CART_F4SC:
        if (a >= 0xFF4 && a <= 0xFFB) select_bank((u8)(a - 0xFF4));
        break;
    case CART_FA:
        if (a >= 0xFF8 && a <= 0xFFA) select_bank((u8)(a - 0xFF8));
        break;
    case CART_E0:
        if (a >= 0xFE0 && a <= 0xFF7)
            cart.seg[(a - 0xFE0) >> 3] = cart.rom + (u32)(a & 7) * 1024;
        break;
    case CART_E7:
        if (a >= 0xFE0 && a <= 0xFE7) { cart.e7_rom_bank = (u8)(a & 7); e7_map(); }
        else if (a >= 0xFE8 && a <= 0xFEB) cart.e7_ram_bank = (u8)(a & 3);
        break;
    default:
        break;
    }
}

u8 cart_read(u16 addr)
{
    u16 a = addr & 0x0FFF;

    switch (cart.type) {
    case CART_2K:
    case CART_4K:
    case CART_3F:
        break;
    case CART_FE:
        return cart.rom[a + ((addr & 0x2000) ? 0 : 4096)];
    case CART_F8SC: case CART_F6SC: case CART_F4SC:
        if (a < 0x100) return cart.ram[a & 0x7F];
        if (a >= 0xFF4) hotspot(a);
        break;
    case CART_FA:
        if (a < 0x200) return cart.ram[a & 0xFF];
        if (a >= 0xFF8) hotspot(a);
        break;
    case CART_E7:
        if (a >= 0xFE0) hotspot(a);
        if (a < 0x800 && cart.e7_rom_bank == 7)
            return cart.ram[a & 0x3FF];                           /* 1K RAM */
        if (a >= 0x800 && a < 0xA00)
            return cart.ram[1024 + cart.e7_ram_bank * 256 + (a & 0xFF)];
        break;
    default:
        if (a >= 0xFE0) hotspot(a);
        break;
    }
    return cart.seg[a >> 10][a & 0x3FF];
}

void cart_write(u16 addr, u8 val)
{
    u16 a = addr & 0x0FFF;

    switch (cart.type) {
    case CART_F8SC: case CART_F6SC: case CART_F4SC:
        if (a < 0x80) { cart.ram[a] = val; return; }
        break;
    case CART_FA:
        if (a < 0x100) { cart.ram[a] = val; return; }
        break;
    case CART_E7:
        if (a < 0x400 && cart.e7_rom_bank == 7) { cart.ram[a] = val; return; }
        if (a >= 0x800 && a < 0x900) {
            cart.ram[1024 + cart.e7_ram_bank * 256 + (a & 0xFF)] = val;
            return;
        }
        break;
    default:
        break;
    }
    if (a >= 0xFE0) hotspot(a);
}

void cart_tia_write(u16 addr, u8 val)
{
    if (cart.type == CART_3F && (addr & 0x1FFF) < 0x40) {
        cart.bank = val;
        threef_map();
    }
}

#ifdef A26_ASM_CPU
/* ---- read map for the assembler core --------------------------------- */

static const u8 *map_seg[4];
static u8 map_e7_bank;

/* pages that must go through cart_read(): hotspots and cartridge RAM */
static int slow_page(u16 a)
{
    switch (cart.type) {
    case CART_F8SC: case CART_F6SC: case CART_F4SC:
        return a < 0x100 || a == 0xF00;
    case CART_FA:
        return a < 0x200 || a == 0xF00;
    case CART_E7:
        return (a < 0x800 && cart.e7_rom_bank == 7) || (a >= 0x800 && a < 0xA00) || a == 0xF00;
    case CART_F8: case CART_F6: case CART_F4: case CART_E0:
        return a == 0xF00;
    default:
        return 0;
    }
}

void cart_build_asm_map(u32 *map)
{
    int p;
    for (p = 0; p < 256; p++) {
        u16 addr = (u16)(p << 8);
        u16 a = addr & 0x0FFF;
        const u8 *ptr;
        if (!(addr & 0x1000) || slow_page(a)) {
            map[p] = 0;
            continue;
        }
        if (cart.type == CART_FE)
            ptr = cart.rom + a + ((addr & 0x2000) ? 0 : 4096);
        else
            ptr = cart.seg[a >> 10] + (a & 0x3FF);
        /* biased so that map[p] + (s16)address points at the byte */
        map[p] = (u32)(unsigned long)ptr - (u32)(s32)(s16)addr;
    }
    for (p = 0; p < 4; p++) map_seg[p] = cart.seg[p];
    map_e7_bank = cart.e7_rom_bank;
}

int cart_asm_map_changed(void)
{
    return cart.seg[0] != map_seg[0] || cart.seg[1] != map_seg[1] ||
           cart.seg[2] != map_seg[2] || cart.seg[3] != map_seg[3] ||
           cart.e7_rom_bank != map_e7_bank;
}
#endif
