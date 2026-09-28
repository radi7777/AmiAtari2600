; tia_offs.i - Tia struct offsets for the 68k renderer and write path
; (the C struct is in src/core/tia.h; tia.c checks these at compile time)

T_LINE    equ 8
T_PF      equ 16
T_P0M     equ 20
T_P1M     equ 24
T_M0M     equ 28
T_M1M     equ 32
T_BLM     equ 36
T_RUNS    equ 40
T_PRIO    equ 60
T_CFV     equ 64
T_CLV     equ 68
T_COLL    equ 72
T_NRUNS   equ 74
T_GP0     equ 79
T_GP1     equ 80
T_M0ON    equ 81
T_M1ON    equ 82
T_BLON    equ 83
T_COLL_L  equ 84
T_COLR    equ 89
T_VBLANK  equ 94
T_CTRLPF  equ 95
T_COLUBK  equ 96
T_HMB     equ 97
T_POS     equ 98                    ; P0, P1, M0, M1, BL

C_CYC     equ 24                    ; AsmCpu (cpu_asm.h)
C_TARGET  equ 28
T_LSC     equ 0
T_LAST    equ 4
T_FB      equ 12
T_COLUP0  equ 127
T_COLUP1  equ 128
T_COLUPF  equ 129
T_REFP0   equ 130
T_REFP1   equ 131
T_PF0     equ 132
T_PF1     equ 133
T_PF2     equ 134
T_GRP0N   equ 135
T_GRP0O   equ 136
T_GRP1N   equ 137
T_GRP1O   equ 138
T_ENAM0   equ 139
T_ENAM1   equ 140
T_ENABLN  equ 141
T_ENABLO  equ 142
T_VDELP0  equ 148
T_VDELP1  equ 149
T_VDELBL  equ 150
T_RESMP0  equ 151
T_RESMP1  equ 152
LINE_CC   equ 228
HBLANK    equ 68
FB_LINES  equ 320

