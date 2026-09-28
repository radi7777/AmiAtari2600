/*
 * m68kprof.c - count 68k instructions of the emulator per function.
 *
 *   m68kprof bench.exe bench.map rom.bin [-frames N] [-skip N] [-video]
 *            [-cpu 030|040] [-n TOP]
 *
 * Loads an Amiga hunk executable built from bench.c (vbcc, like the real
 * program), runs it on the Musashi 68k emulator and counts executed
 * instructions per function (names from the vlink map). The first -skip
 * frames (default 10) are not counted. Instruction counts are exact and
 * reproducible; cycles are Musashi's estimate and only a rough guide.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m68k.h"

#define MEM_SIZE (16u << 20)
static unsigned char mem[MEM_SIZE];
static unsigned code_base, code_size;
static unsigned long long *hits;
static int counting, stop;
static unsigned long long instr_total;
static unsigned skip_frames = 10;

#define PARAM 0x100
#define ROM_ADDR 0x00E00000u

static unsigned rd32(unsigned a) { return (unsigned)mem[a] << 24 | (unsigned)mem[a + 1] << 16 | (unsigned)mem[a + 2] << 8 | mem[a + 3]; }
static void wr32(unsigned a, unsigned v) { mem[a] = v >> 24; mem[a + 1] = v >> 16; mem[a + 2] = v >> 8; mem[a + 3] = v; }

unsigned int m68k_read_memory_8(unsigned int a) { return mem[a & (MEM_SIZE - 1)]; }
unsigned int m68k_read_memory_16(unsigned int a) { a &= MEM_SIZE - 1; return mem[a] << 8 | mem[a + 1]; }
unsigned int m68k_read_memory_32(unsigned int a) { return rd32(a & (MEM_SIZE - 1)); }
void m68k_write_memory_8(unsigned int a, unsigned int v)
{
    a &= MEM_SIZE - 1;
    mem[a] = v;
}
void m68k_write_memory_16(unsigned int a, unsigned int v) { a &= MEM_SIZE - 1; mem[a] = v >> 8; mem[a + 1] = v; }
void m68k_write_memory_32(unsigned int a, unsigned int v)
{
    a &= MEM_SIZE - 1;
    wr32(a, v);
    if (a == PARAM + 7 * 4 && v >= skip_frames) counting = 1;     /* P_MARK */
}
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }

void prof_hook(unsigned int pc)
{
    if (!counting) return;
    instr_total++;
    if (pc - code_base < code_size) hits[pc - code_base]++;
}

int prof_illegal(int opcode)
{
    (void)opcode;
    stop = 1;
    m68k_end_timeslice();
    return 1;
}

/* ---- Amiga hunk loader ---- */
static unsigned char *file;
static long fpos, flen;
static unsigned get32(void) { unsigned v = (unsigned)file[fpos] << 24 | file[fpos + 1] << 16 | file[fpos + 2] << 8 | file[fpos + 3]; fpos += 4; return v; }

static int load_hunks(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned nhunks, first, last, i, h, base[64], size[64], addr = 0x10000;
    if (!f) return -1;
    fseek(f, 0, SEEK_END); flen = ftell(f); fseek(f, 0, SEEK_SET);
    file = malloc(flen); fread(file, 1, flen, f); fclose(f);
    if (get32() != 0x3F3) return -2;
    while (get32()) ;                           /* resident library names */
    nhunks = get32(); first = get32(); last = get32();
    (void)nhunks;
    for (i = first; i <= last; i++) {
        size[i] = (get32() & 0x3FFFFFFF) * 4;
        base[i] = addr;
        addr = (addr + size[i] + 15) & ~15u;
    }
    h = first;
    while (fpos < flen && h <= last) {
        unsigned type = get32() & 0x3FFFFFFF, n, k;
        switch (type) {
        case 0x3E9: case 0x3EA:                 /* CODE, DATA */
            n = get32() * 4;
            memcpy(mem + base[h], file + fpos, n);
            fpos += n;
            if (type == 0x3E9 && !code_size) { code_base = base[h]; code_size = size[h]; }
            break;
        case 0x3EB: get32(); break;             /* BSS (memory is zeroed) */
        case 0x3EC:                             /* RELOC32 */
            while ((n = get32())) {
                unsigned target = get32();
                for (k = 0; k < n; k++) { unsigned off = get32() + base[h]; wr32(off, rd32(off) + base[target]); }
            }
            break;
        case 0x3FC: case 0x3F7: {               /* RELOC32SHORT */
            unsigned count = 0;
            for (;;) {
                unsigned short cnt, tgt;
                cnt = file[fpos] << 8 | file[fpos + 1]; fpos += 2;
                if (!cnt) break;
                tgt = file[fpos] << 8 | file[fpos + 1]; fpos += 2;
                count += 2;
                for (k = 0; k < cnt; k++) {
                    unsigned off = (file[fpos] << 8 | file[fpos + 1]) + base[h]; fpos += 2; count++;
                    wr32(off, rd32(off) + base[tgt]);
                }
            }
            count++;
            if (count & 1) fpos += 2;           /* long alignment */
            break;
        }
        case 0x3F0:                             /* SYMBOL */
            while ((n = get32())) fpos += (n & 0xFFFFFF) * 4 + 4;
            break;
        case 0x3F1: n = get32(); fpos += n * 4; break;   /* DEBUG */
        case 0x3F2: h++; break;                 /* END */
        default: fprintf(stderr, "unknown hunk type %x\n", type); return -3;
        }
    }
    return 0;
}

/* ---- map ---- */
typedef struct { unsigned off; char name[64]; } Sym;
static Sym *syms; static int nsyms;
static int load_map(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512]; int in_code = 0, cap = 0;
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        unsigned a; char name[256];
        if (!strncmp(line, "Symbols of ", 11)) { in_code = !strncmp(line + 11, "CODE:", 5); continue; }
        if (!in_code || sscanf(line, " 0x%x %255[^:]:", &a, name) != 2) continue;
        if (name[0] == 'l' && name[1] >= '0' && name[1] <= '9') continue;
        if (nsyms == cap) { cap = cap ? cap * 2 : 1024; syms = realloc(syms, cap * sizeof(Sym)); }
        syms[nsyms].off = a;
        strncpy(syms[nsyms].name, name, 63); syms[nsyms].name[63] = 0;
        nsyms++;
    }
    fclose(f);
    return 0;
}
static int cmp_sym(const void *a, const void *b) { const Sym *x = a, *y = b; return x->off < y->off ? -1 : x->off > y->off; }
typedef struct { const char *name; unsigned long long n; } Row;
static int cmp_row(const void *a, const void *b) { const Row *x = a, *y = b; return x->n < y->n ? 1 : x->n > y->n ? -1 : 0; }

int main(int argc, char **argv)
{
    unsigned frames = 60, video = 0, top = 30, i, cpu = M68K_CPU_TYPE_68030;
    long rsize; FILE *rf;
    int a;
    if (argc < 4) { fprintf(stderr, "usage: m68kprof bench.exe bench.map rom.bin [-frames N] [-skip N] [-video] [-cpu 030|040] [-n TOP]\n"); return 2; }
    for (a = 4; a < argc; a++) {
        if (!strcmp(argv[a], "-frames")) frames = atoi(argv[++a]);
        else if (!strcmp(argv[a], "-skip")) skip_frames = atoi(argv[++a]);
        else if (!strcmp(argv[a], "-video")) video = 1;
        else if (!strcmp(argv[a], "-n")) top = atoi(argv[++a]);
        else if (!strcmp(argv[a], "-cpu")) cpu = !strcmp(argv[++a], "040") ? M68K_CPU_TYPE_68040 : M68K_CPU_TYPE_68030;
    }
    if (load_hunks(argv[1])) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
    if (load_map(argv[2])) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    qsort(syms, nsyms, sizeof(Sym), cmp_sym);
    rf = fopen(argv[3], "rb");
    if (!rf) { fprintf(stderr, "cannot read %s\n", argv[3]); return 1; }
    rsize = fread(mem + ROM_ADDR, 1, 1 << 20, rf); fclose(rf);
    wr32(PARAM + 0, rsize); wr32(PARAM + 4, frames + skip_frames); wr32(PARAM + 8, video); wr32(PARAM + 12, ROM_ADDR);
    hits = calloc(code_size, sizeof(*hits));

    wr32(0, 0x00F00000); wr32(4, code_base);   /* reset vectors: SSP, PC */
    m68k_init();
    m68k_set_cpu_type(cpu);
    m68k_pulse_reset();
    {
        unsigned long long cycles = 0;
        while (!stop) {
            int c = m68k_execute(100000);
            if (counting) cycles += c;
        }
        printf("%u frames (after %u skipped): %.0f instructions/frame, %.0f cycles/frame (Musashi estimate)\n",
               frames, skip_frames, (double)instr_total / frames, (double)cycles / frames);
        printf("lines/frame %u, converted lines/frame %.1f, chip longword writes/frame %.1f\n",
               rd32(PARAM + 16), (double)rd32(PARAM + 20) / (frames + skip_frames),
               (double)rd32(PARAM + 24) / (frames + skip_frames));
    }
    {
        Row *rows = calloc(nsyms + 1, sizeof(Row));
        int k = -1, nrows = 0;
        unsigned long long sum = 0;
        for (i = 0; i < code_size; i++) {
            if (!hits[i]) continue;
            while (k + 1 < nsyms && syms[k + 1].off <= i) k++;
            if (nrows == 0 || rows[nrows - 1].name != (k >= 0 ? syms[k].name : "?")) {
                int j, found = -1;
                const char *nm = k >= 0 ? syms[k].name : "?";
                for (j = 0; j < nrows; j++) if (rows[j].name == nm) { found = j; break; }
                if (found < 0) { rows[nrows].name = nm; rows[nrows].n = 0; found = nrows++; }
                rows[found].n += hits[i];
            } else {
                rows[nrows - 1].n += hits[i];
            }
            sum += hits[i];
        }
        qsort(rows, nrows, sizeof(Row), cmp_row);
        for (i = 0; i < top && i < (unsigned)nrows; i++)
            printf("%6.2f%%  %9.0f/frame  %s\n", 100.0 * rows[i].n / sum, (double)rows[i].n / frames, rows[i].name);
    }
    return 0;
}
