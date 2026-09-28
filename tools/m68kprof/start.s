; start.s - entry of the freestanding benchmark (bench.c), run by m68kprof
        xref    _main
        section CODE,code
_start:
        jsr     _main
        illegal                 ; ends the run in m68kprof
