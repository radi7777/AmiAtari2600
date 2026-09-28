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
T_CFV     equ 96
T_CLV     equ 100
T_COLL    equ 104
T_NRUNS   equ 64
T_GP0     equ 69
T_GP1     equ 70
T_M0ON    equ 71
T_M1ON    equ 72
T_BLON    equ 73
T_COLL_L  equ 74
T_COLR    equ 79
T_VBLANK  equ 84
T_CTRLPF  equ 85
T_COLUBK  equ 86
T_HMB     equ 87
T_POS     equ 88                    ; P0, P1, M0, M1, BL

C_CYC     equ 24                    ; AsmCpu (cpu_asm.h)
C_TARGET  equ 28
T_LSC     equ 0
T_LAST    equ 4
T_FB      equ 12
T_COLUP0  equ 131
T_COLUP1  equ 132
T_COLUPF  equ 133
T_REFP0   equ 134
T_REFP1   equ 135
T_PF0     equ 136
T_PF1     equ 137
T_PF2     equ 138
T_GRP0N   equ 139
T_GRP0O   equ 140
T_GRP1N   equ 141
T_GRP1O   equ 142
T_ENAM0   equ 143
T_ENAM1   equ 144
T_ENABLN  equ 145
T_ENABLO  equ 146
T_VDELP0  equ 152
T_VDELP1  equ 153
T_VDELBL  equ 154
T_RESMP0  equ 155
T_RESMP1  equ 156
T_HMP0    equ 147                   ; HMP0, HMP1, HMM0, HMM1, HMBL
T_HMDISP  equ 159                   ; s8 [5]
T_HMPEND  equ 164
T_HMW     equ 165
T_HMLCC   equ 166                   ; u32 (word aligned)
T_HMV     equ 170                   ; u8 [5]
T_HMLOCK  equ 175                   ; u8 [5]
LINE_CC   equ 228
HBLANK    equ 68
FB_LINES  equ 320

T_SEG     equ 106
