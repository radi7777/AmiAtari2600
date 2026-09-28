/*
 * unzip_test.c - src/core/unzip.c against real zip files.
 *
 *   unzip_test archive.zip original.bin    exit code 0 = same bytes
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/core/unzip.h"

int main(int argc, char **argv)
{
    ZipEntry e;
    FILE *f;
    u8 *orig, *data;
    long n;
    if (argc != 3) {
        fprintf(stderr, "usage: %s archive.zip original\n", argv[0]);
        return 2;
    }
    if (!zip_is_zip(argv[1])) { printf("FAIL %s: not a .zip name\n", argv[1]); return 1; }
    if (zip_find_rom(argv[1], &e)) { printf("FAIL %s: no ROM found\n", argv[1]); return 1; }
    f = fopen(argv[2], "rb");
    if (!f) { printf("FAIL cannot read %s\n", argv[2]); return 1; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    orig = (u8 *)malloc((size_t)n);
    if (!orig || fread(orig, 1, (size_t)n, f) != (size_t)n) { printf("FAIL read\n"); return 1; }
    fclose(f);
    if ((long)e.usize != n) { printf("FAIL %s: size %lu, want %ld\n", argv[1], (unsigned long)e.usize, n); return 1; }
    if (e.crc != zip_crc32(0, orig, (u32)n)) { printf("FAIL %s: directory CRC\n", argv[1]); return 1; }
    data = zip_extract(argv[1], &e);
    if (!data) { printf("FAIL %s: extract (method %u)\n", argv[1], e.method); return 1; }
    if (memcmp(data, orig, (size_t)n)) { printf("FAIL %s: bytes differ\n", argv[1]); return 1; }
    free(data);
    free(orig);
    return 0;
}
