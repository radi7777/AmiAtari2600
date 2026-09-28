/*
 * AmiAtari2600 - Atari 2600 emulator for classic Amiga (68030/ECS)
 *
 * types.h - basic integer types for the portable core.
 *
 * The core must build with vbcc (C89 + a few C99 bits) as well as with
 * gcc/clang on the host, so we avoid <stdint.h> where it is not available.
 */
#ifndef A26_TYPES_H
#define A26_TYPES_H

#if defined(__VBCC__) && !defined(__STDC_VERSION__)
typedef unsigned char  u8;
typedef signed char    s8;
typedef unsigned short u16;
typedef signed short   s16;
typedef unsigned long  u32;
typedef signed long    s32;
#else
#include <stdint.h>
typedef uint8_t  u8;
typedef int8_t   s8;
typedef uint16_t u16;
typedef int16_t  s16;
typedef uint32_t u32;
typedef int32_t  s32;
#endif

/* Register-parameter hints for vbcc (see docs/PROJEKT.md, "__reg()").
 * On other compilers they expand to nothing. */
#ifdef __VBCC__
#define REG(r) __reg(r)
#define INLINE
#else
#define REG(r)
#define INLINE static inline
#endif

#endif
