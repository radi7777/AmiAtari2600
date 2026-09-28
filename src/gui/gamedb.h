/*
 * gamedb.h - game information from the libretro databases (clrmamepro .dat
 * files, keyed by the CRC32 of the ROM): No-Intro names and regions,
 * publisher, developer, release year and genre.
 */
#ifndef A26_GAMEDB_H
#define A26_GAMEDB_H

typedef unsigned long crc_t;

typedef struct GameInfo {
    struct GameInfo *next;      /* hash chain */
    crc_t crc;
    char *name;                 /* No-Intro name, e.g. "River Raid (USA)" */
    char *region;
    char *publisher;
    char *developer;
    char *year;
    char *genre;
} GameInfo;

crc_t crc32_buf(crc_t crc, const unsigned char *p, long n);

/* load all .dat files present in dir (missing ones are skipped);
 * returns the number of games */
int  gamedb_load(const char *dir);
void gamedb_free(void);
const GameInfo *gamedb_find(crc_t crc);

/* file name of the libretro thumbnail for a No-Intro name: the characters
 * &*\/:`<>?\\|" are replaced by '_' (libretro convention) */
void gamedb_thumb_name(const char *name, char *out, int outlen);
/* the same, URL-encoded for https://raw.githubusercontent.com/... */
void gamedb_thumb_url(const char *name, char *out, int outlen);

/* database files and where they come from */
#define GAMEDB_FILES 5
extern const char *const gamedb_file[GAMEDB_FILES];
extern const char *const gamedb_url[GAMEDB_FILES];

#endif
