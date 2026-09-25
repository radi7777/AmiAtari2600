/*
 * cart.h - cartridge ROM + bankswitching schemes.
 */
#ifndef A26_CART_H
#define A26_CART_H

#include "types.h"

typedef enum {
    CART_UNKNOWN = 0,
    CART_2K,        /* 2K, mirrored */
    CART_4K,        /* 4K, no switching */
    CART_F8,        /* 8K Atari, hotspots $1FF8/$1FF9 */
    CART_F6,        /* 16K Atari, $1FF6-$1FF9 */
    CART_F4,        /* 32K Atari, $1FF4-$1FFB */
    CART_F8SC,      /* F8 + 128 bytes Superchip RAM */
    CART_F6SC,
    CART_F4SC,
    CART_FA,        /* 12K CBS RAM+, 256 bytes RAM */
    CART_E0,        /* 8K Parker Bros., 4 x 1K slices */
    CART_E7,        /* 16K M-Network, 2K RAM */
    CART_3F,        /* Tigervision, 2K banks selected via $3F */
    CART_FE,        /* 8K Activision (bank follows A13 of the CPU address) */
    CART_TYPE_COUNT
} CartType;

typedef struct {
    CartType type;
    u8  *rom;           /* ROM image (owned by caller) */
    u32 size;
    u8  ram[2048];      /* extra RAM (Superchip, CBS, M-Network) */
    const u8 *seg[4];   /* 1K read segments for $1000-$1FFF */
    u8  bank;           /* current bank (for simple schemes) */
    u8  e7_ram_bank;    /* M-Network 256 byte RAM bank */
    u8  e7_rom_bank;
} Cart;

extern Cart cart;

CartType    cart_detect(const u8 *rom, u32 size);
const char *cart_type_name(CartType t);
CartType    cart_type_from_name(const char *name);
int         cart_init(u8 *rom, u32 size, CartType type);  /* 0 = ok */
void        cart_reset(void);

u8   cart_read(u16 addr);
void cart_write(u16 addr, u8 val);
/* 3F scheme: bank select by writing to TIA space $00-$3F */
extern int  cart_tia_hook;
void cart_tia_write(u16 addr, u8 val);

#ifdef A26_ASM_CPU
/* read map for the assembler CPU core (see cpu_asm.h) */
void cart_build_asm_map(u32 *map);
int  cart_asm_map_changed(void);
#endif

#endif
