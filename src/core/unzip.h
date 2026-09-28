/*
 * unzip.h - ROMs straight from .zip files (as romsets come).
 *
 * zip_find_rom() reads only the central directory, so the ROM's CRC32
 * and size are known without unpacking (quick scanning of a whole
 * romset). zip_extract() unpacks it (stored or deflate) and checks the
 * CRC. Portable C, no other dependencies.
 */
#ifndef A26_UNZIP_H
#define A26_UNZIP_H

#include "types.h"

typedef struct {
    char name[128];             /* file name inside the archive */
    u32  crc;                   /* CRC32 from the directory */
    u32  csize, usize;          /* packed / unpacked size */
    u32  offset;                /* of the local header */
    u16  method;                /* 0 stored, 8 deflate */
} ZipEntry;

/* is name "*.zip" (any case)? */
int  zip_is_zip(const char *name);
/* the ROM in the archive: the first file of 2 KB..512 KB, preferring
 * .bin/.a26/.rom; 0 = found */
int  zip_find_rom(const char *path, ZipEntry *e);
/* unpacks entry e; malloc'ed buffer of e->usize bytes or NULL (also on
 * a CRC mismatch) */
u8  *zip_extract(const char *path, const ZipEntry *e);

/* raw deflate data -> out (exactly outlen bytes); 0 = ok */
int  zip_inflate(const u8 *in, u32 inlen, u8 *out, u32 outlen);
u32  zip_crc32(u32 crc, const u8 *p, u32 n);

#endif
