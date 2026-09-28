/*
 * gamedb.c - libretro database parser, CRC32, thumbnail names.
 * Portable C (no AmigaOS calls); the .dat files are plain text:
 *
 *   game (
 *       name "River Raid (USA)"            (No-Intro file)
 *       comment "River Raid (USA)"         (metadata files)
 *       publisher "Activision"
 *       rom ( name "..." size 4096 crc 1E86DE5A md5 ... )
 *   )
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gamedb.h"

#define BASE "https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/"

const char *const gamedb_file[GAMEDB_FILES] = {
    "nointro.dat", "publisher.dat", "developer.dat", "releaseyear.dat", "genre.dat"
};
const char *const gamedb_url[GAMEDB_FILES] = {
    BASE "no-intro/Atari%20-%202600.dat",
    BASE "publisher/Atari%20-%202600.dat",
    BASE "developer/Atari%20-%202600.dat",
    BASE "releaseyear/Atari%20-%202600.dat",
    BASE "genre/Atari%20-%202600.dat"
};

/* ---- CRC32 (as in zip/No-Intro) ---- */
static crc_t crc_table[256];

crc_t crc32_buf(crc_t crc, const unsigned char *p, long n)
{
    if (!crc_table[1]) {
        crc_t c;
        int i, k;
        for (i = 0; i < 256; i++) {
            c = (crc_t)i;
            for (k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    }
    crc = ~crc & 0xFFFFFFFFUL;
    while (n-- > 0)
        crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc & 0xFFFFFFFFUL;
}

/* ---- hash table ---- */
#define HASH 256
static GameInfo *hash[HASH];

static GameInfo *get(crc_t crc, int create)
{
    GameInfo *g;
    for (g = hash[crc & (HASH - 1)]; g; g = g->next)
        if (g->crc == crc) return g;
    if (!create) return NULL;
    g = (GameInfo *)calloc(1, sizeof(GameInfo));
    if (!g) return NULL;
    g->crc = crc;
    g->next = hash[crc & (HASH - 1)];
    hash[crc & (HASH - 1)] = g;
    return g;
}

const GameInfo *gamedb_find(crc_t crc)
{
    return get(crc, 0);
}

void gamedb_free(void)
{
    int i;
    for (i = 0; i < HASH; i++) {
        GameInfo *g = hash[i];
        while (g) {
            GameInfo *n = g->next;
            free(g->name); free(g->region); free(g->publisher);
            free(g->developer); free(g->year); free(g->genre);
            free(g);
            g = n;
        }
        hash[i] = NULL;
    }
}

static char *dupstr(const char *s)
{
    char *d = (char *)malloc(strlen(s) + 1);
    if (d) strcpy(d, s);
    return d;
}

/* value of  key "..."  in line, or NULL */
static int quoted(const char *line, const char *key, char *out, int outlen)
{
    const char *p = strstr(line, key);
    int n = 0;
    if (!p) return 0;
    p += strlen(key);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    while (*p && *p != '"' && n < outlen - 1) out[n++] = *p++;
    out[n] = 0;
    return 1;
}

static int load_file(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512], name[256], region[64], value[256], field[16];
    int count = 0, have_value = 0;
    static const char *const fields[] = { "publisher", "developer", "releaseyear", "genre", NULL };
    int fi;
    if (!f) return 0;
    name[0] = region[0] = value[0] = field[0] = 0;
    while (fgets(line, sizeof(line), f)) {
        char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (!strncmp(t, "game (", 6)) {
            name[0] = region[0] = 0;
            have_value = 0;
            continue;
        }
        if (!strncmp(t, "name \"", 6)) { quoted(t, "name", name, sizeof(name)); continue; }
        /* metadata files name the game in a comment: fallback name */
        if (!strncmp(t, "comment \"", 9)) { quoted(t, "comment", name, sizeof(name)); continue; }
        if (!strncmp(t, "region \"", 8)) { quoted(t, "region", region, sizeof(region)); continue; }
        for (fi = 0; fields[fi]; fi++) {
            size_t l = strlen(fields[fi]);
            if (!strncmp(t, fields[fi], l) && t[l] == ' ') {
                if (quoted(t, fields[fi], value, sizeof(value))) {
                    strcpy(field, fields[fi]);
                    have_value = 1;
                }
            }
        }
        if (!strncmp(t, "rom (", 5)) {
            char *c = strstr(t, " crc ");
            GameInfo *g;
            if (!c) continue;
            g = get(strtoul(c + 5, NULL, 16), 1);
            if (!g) continue;
            if (name[0] && !g->name) g->name = dupstr(name);
            if (region[0] && !g->region) g->region = dupstr(region);
            if (have_value) {
                char **dst = !strcmp(field, "publisher") ? &g->publisher :
                             !strcmp(field, "developer") ? &g->developer :
                             !strcmp(field, "releaseyear") ? &g->year : &g->genre;
                if (!*dst) *dst = dupstr(value);
            }
            count++;
        }
    }
    fclose(f);
    return count;
}

int gamedb_load(const char *dir)
{
    char path[512];
    int i, n = 0;
    gamedb_free();
    for (i = 0; i < GAMEDB_FILES; i++) {
        size_t l;
        strncpy(path, dir, sizeof(path) - 40);
        path[sizeof(path) - 40] = 0;
        l = strlen(path);
        if (l && path[l - 1] != ':' && path[l - 1] != '/') strcat(path, "/");
        strcat(path, gamedb_file[i]);
        if (i == 0) n = load_file(path);
        else load_file(path);
    }
    return n;
}

void gamedb_thumb_name(const char *name, char *out, int outlen)
{
    int n = 0;
    for (; *name && n < outlen - 1; name++)
        out[n++] = strchr("&*/:`<>?\\|\"", *name) ? '_' : *name;
    out[n] = 0;
}

void gamedb_thumb_url(const char *name, char *out, int outlen)
{
    static const char hex[] = "0123456789ABCDEF";
    char tn[256];
    const char *p;
    int n;
    gamedb_thumb_name(name, tn, sizeof(tn));
    n = sprintf(out, "https://raw.githubusercontent.com/libretro-thumbnails/Atari_-_2600/master/Named_Snaps/");
    for (p = tn; *p && n < outlen - 8; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '(' || c == ')' || c == '!') {
            out[n++] = (char)c;
        } else {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    strcpy(out + n, ".png");
}
