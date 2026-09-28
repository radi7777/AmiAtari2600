; cpu6507.s - 6507 interpreter core in 68020+/68030 assembler (vasm, mot syntax)
;
; Hybrid core: the common opcodes are implemented here, everything else
; (decimal mode ADC/SBC, undocumented opcodes, BRK/RTI, rare addressing
; modes) falls back to the C core for exactly one instruction via
; AsmCpu.step. That keeps the core complete from day one; more opcodes can
; be moved into assembler one at a time (see "fallback" in the table).
;
; Build:  vasmm68k_mot -m68030 -Fhunk -o cpu6507.o cpu6507.s   (Amiga)
;         vasmm68k_mot -m68030 -Felf  -o cpu6507.o cpu6507.s   (qemu tests)
;
; C prototype: void asmcpu_exec(AsmCpu *ctx);  (see src/core/cpu_asm.h)
;
; Registers while running:
;   d0,d1,a0,a1  scratch (a0/a1/d0/d1 are also clobbered by C callbacks)
;   d2 = A   d3 = X   d4 = Y    (upper 24 bits always zero)
;   d5 = cycle counter (a26_cycles)
;   d6 = N source (bit 7)   d7 = Z source (Z set when byte == 0)
;   a2 = opcode table   a3 = PC as host pointer (see cpu_asm.h)   a4 = AsmCpu
;   a5 = read map       a6 = ram_base
;
; Timing: every memory access adds one cycle (fast paths do addq.l #1,d5,
; slow paths let the C bus code count). Internal/dummy cycles are added
; before the final write of an instruction, as in cpu.c.

; ---- AsmCpu offsets (keep in sync with cpu_asm.h) ----
C_A      equ 0
C_X      equ 4
C_Y      equ 8
C_S      equ 12
C_SREG   equ 15          ; low byte of s
C_PC     equ 16
C_P      equ 20
C_CYC    equ 24
C_TARGET equ 28
C_RD     equ 32
C_WR     equ 36
C_STEP   equ 40
C_RAM    equ 44
C_STK    equ 48
C_FC     equ 52
C_FV     equ 53
C_FD     equ 54
C_FI     equ 55
C_MIRROR equ 56
C_TMP    equ 58          ; word: TMP = high byte, TMP+1 = low byte
C_TMP2   equ 60
C_PCBIAS equ 64          ; PC = (a3 - C_PCBIAS) & $FFFF
C_PCEND  equ 68          ; end of the current fast code page, 0 = slow mode
C_TIADIR equ 63          ; byte: TIA writes may use C_TIAWR
C_MAP    equ 72
C_TIAWR  equ 1096        ; void tiawr(u32 addr, u32 val)
C_PCHOST equ 1100        ; a3 at slow reads (RIOT timer wait skip)

; ======================================================================
; macros
; ======================================================================

; read byte at address d0.w -> d0.l (zero-extended). Trashes d1, a0.
READ    macro
        move.w  d0,d1
        lsr.w   #8,d1
        move.l  (a5,d1.w*4),a0
        move.l  a0,d1
        beq.s   .s\@
        moveq   #0,d1
        move.b  (a0,d0.w),d1
        move.l  d1,d0
        addq.l  #1,d5
        bra.s   .d\@
.s\@:
        bsr     slow_read
.d\@:
        endm

; fetch the next code byte -> d0.l. Fast path: a3 points into the ROM.
FETCHB  macro
        cmp.l   C_PCEND(a4),a3
        bhs.s   .s\@
        moveq   #0,d0
        move.b  (a3)+,d0
        addq.l  #1,d5
        bra.s   .d\@
.s\@:
        bsr     fetch_slow
.d\@:
        endm

; current 6502 PC -> d0.l
GETPC   macro
        move.l  a3,d0
        sub.l   C_PCBIAS(a4),d0
        and.l   #$ffff,d0
        endm

; jump: d0.l = new PC (0..$FFFF). The next fetch looks up the page.
SETPC   macro
        move.l  d0,a3
        clr.l   C_PCBIAS(a4)
        clr.l   C_PCEND(a4)
        endm

; fetch 16 bit operand at PC -> d0.l
FETCHW  macro
        FETCHB
        move.b  d0,C_TMP+1(a4)
        FETCHB
        move.b  d0,C_TMP(a4)
        moveq   #0,d0
        move.w  C_TMP(a4),d0
        endm

; zero page read: d0 = address $00-$FF -> d0.l value
ZPREAD  macro
        tst.b   d0
        bpl.s   .s\@
        moveq   #0,d1
        move.b  (a6,d0.w),d1
        move.l  d1,d0
        addq.l  #1,d5
        bra.s   .d\@
.s\@:
        bsr     slow_read
.d\@:
        endm

; zero page write: d0 = address, d1 = value
ZPWRITE macro
        tst.b   d0
        bpl.s   .s\@
        move.b  d1,(a6,d0.w)
        addq.l  #1,d5
        bra.s   .d\@
.s\@:
        bsr     zp_tia_write
.d\@:
        endm

; any other write: d0 = address, d1 = value
WRITE   macro
        bsr     slow_write
        endm

SETNZ   macro
        move.b  \1,d6
        move.b  \1,d7
        endm

; push d1.b (trashes d0, a0)
PUSH    macro
        moveq   #0,d0
        move.b  C_SREG(a4),d0
        bpl.s   .s\@
        move.l  C_STK(a4),a0
        move.b  d1,(a0,d0.w)
        addq.l  #1,d5
        bra.s   .d\@
.s\@:
        or.w    #$100,d0
        bsr     slow_write
.d\@:
        subq.b  #1,C_SREG(a4)
        endm

; pull -> d0.l (trashes d1, a0)
PULL    macro
        addq.b  #1,C_SREG(a4)
        moveq   #0,d0
        move.b  C_SREG(a4),d0
        bpl.s   .s\@
        move.l  C_STK(a4),a0
        moveq   #0,d1
        move.b  (a0,d0.w),d1
        move.l  d1,d0
        addq.l  #1,d5
        bra.s   .d\@
.s\@:
        or.w    #$100,d0
        bsr     slow_read
.d\@:
        endm

; ---- effective addresses -> d0.l ----
EA_ZPX  macro
        FETCHB
        add.b   d3,d0
        addq.l  #1,d5
        endm

EA_ZPY  macro
        FETCHB
        add.b   d4,d0
        addq.l  #1,d5
        endm

; absolute indexed, read: +1 cycle on page cross. \1 = index register
EA_ABR  macro
        FETCHW
        move.w  d0,d1
        add.w   \1,d0
        eor.w   d0,d1
        and.w   #$ff00,d1
        beq.s   .n\@
        addq.l  #1,d5
.n\@:
        endm

; absolute indexed, write/RMW: always +1 cycle
EA_ABW  macro
        FETCHW
        add.w   \1,d0
        addq.l  #1,d5
        endm

; (zp),Y read
EA_IZYR macro
        FETCHB
        move.b  d0,C_TMP2(a4)
        ZPREAD
        move.b  d0,C_TMP+1(a4)
        moveq   #0,d0
        move.b  C_TMP2(a4),d0
        addq.b  #1,d0
        ZPREAD
        move.b  d0,C_TMP(a4)
        moveq   #0,d0
        move.w  C_TMP(a4),d0
        move.w  d0,d1
        add.w   d4,d0
        eor.w   d0,d1
        and.w   #$ff00,d1
        beq.s   .n\@
        addq.l  #1,d5
.n\@:
        endm

; (zp),Y write
EA_IZYW macro
        FETCHB
        move.b  d0,C_TMP2(a4)
        ZPREAD
        move.b  d0,C_TMP+1(a4)
        moveq   #0,d0
        move.b  C_TMP2(a4),d0
        addq.b  #1,d0
        ZPREAD
        move.b  d0,C_TMP(a4)
        moveq   #0,d0
        move.w  C_TMP(a4),d0
        add.w   d4,d0
        addq.l  #1,d5
        endm

; (zp,X)
EA_IZX  macro
        FETCHB
        add.b   d3,d0
        addq.l  #1,d5
        move.b  d0,C_TMP2(a4)
        ZPREAD
        move.b  d0,C_TMP+1(a4)
        moveq   #0,d0
        move.b  C_TMP2(a4),d0
        addq.b  #1,d0
        ZPREAD
        move.b  d0,C_TMP(a4)
        moveq   #0,d0
        move.w  C_TMP(a4),d0
        endm

; ---- ALU operations on d0 ----
OP_LD   macro                   ; \1 = destination register
        move.b  d0,\1
        SETNZ   d0
        endm

OP_CMP  macro                   ; \1 = register compared with d0
        move.b  \1,d1
        sub.b   d0,d1
        scc     C_FC(a4)
        SETNZ   d1
        endm

OP_ADC  macro
        move.b  C_FC(a4),d1
        add.b   d1,d1           ; X = carry
        addx.b  d0,d2
        scs     C_FC(a4)
        svs     C_FV(a4)
        SETNZ   d2
        endm

OP_SBC  macro
        move.b  C_FC(a4),d1
        not.b   d1
        add.b   d1,d1           ; X = borrow = !carry
        subx.b  d0,d2
        scc     C_FC(a4)
        svs     C_FV(a4)
        SETNZ   d2
        endm

OP_BIT  macro
        move.b  d0,d6
        btst    #6,d0
        sne     C_FV(a4)
        and.b   d2,d0
        move.b  d0,d7
        endm

; decimal mode ADC/SBC go to the C core
NODEC   macro
        tst.b   C_FD(a4)
        bne     fallback
        endm

        section .text,code

        xdef    _asmcpu_exec
        xdef    asmcpu_exec

; ======================================================================
; entry / exit
; ======================================================================
_asmcpu_exec:
asmcpu_exec:
        movem.l d2-d7/a2-a6,-(sp)
        move.l  48(sp),a4
        lea     C_MAP(a4),a5
        move.l  C_RAM(a4),a6
        lea     optable(pc),a2
        bsr     sync_in

loop:
        cmp.l   C_TARGET(a4),d5
        bpl.s   exit
        FETCHB
        move.l  (a2,d0.w*4),a0
        jmp     (a0)

exit:
        bsr     sync_out
        movem.l (sp)+,d2-d7/a2-a6
        rts

sync_in:
        move.l  C_A(a4),d2
        move.l  C_X(a4),d3
        move.l  C_Y(a4),d4
        move.l  C_PC(a4),a3
        clr.l   C_PCBIAS(a4)
        clr.l   C_PCEND(a4)
        move.l  C_CYC(a4),d5
        move.l  C_P(a4),d0
        bra     unpackp

sync_out:
        move.l  d2,C_A(a4)
        move.l  d3,C_X(a4)
        move.l  d4,C_Y(a4)
        GETPC
        move.l  d0,C_PC(a4)
        move.l  d5,C_CYC(a4)
        bsr     packp
        move.l  d0,C_P(a4)
        rts

; flags -> P in d0.l
packp:
        moveq   #$20,d0
        tst.b   d6
        bpl.s   .n
        or.b    #$80,d0
.n:     tst.b   C_FV(a4)
        beq.s   .v
        or.b    #$40,d0
.v:     tst.b   C_FD(a4)
        beq.s   .d
        or.b    #$08,d0
.d:     tst.b   C_FI(a4)
        beq.s   .i
        or.b    #$04,d0
.i:     tst.b   d7
        bne.s   .z
        or.b    #$02,d0
.z:     tst.b   C_FC(a4)
        beq.s   .c
        or.b    #$01,d0
.c:     rts

; P in d0 -> flags
unpackp:
        move.b  d0,d6
        and.b   #$80,d6
        btst    #1,d0
        seq     d7
        btst    #0,d0
        sne     C_FC(a4)
        btst    #6,d0
        sne     C_FV(a4)
        btst    #3,d0
        sne     C_FD(a4)
        btst    #2,d0
        sne     C_FI(a4)
        rts

; ======================================================================
; slow paths (C callbacks)
; ======================================================================

; d0.w = address -> d0.l = value
slow_read:
        and.l   #$ffff,d0
        tst.b   C_MIRROR(a4)
        beq.s   .call
        move.w  d0,d1
        and.w   #$1280,d1       ; A12=0, A9=0, A7=1: RIOT RAM (incl. mirrors)
        cmp.w   #$0080,d1
        bne.s   .call
        and.w   #$ff,d0
        moveq   #0,d1
        move.b  (a6,d0.w),d1
        move.l  d1,d0
        addq.l  #1,d5
        rts
.call:
        move.l  d5,C_CYC(a4)
        move.l  a3,C_PCHOST(a4)
        move.l  d0,-(sp)
        move.l  C_RD(a4),a0
        jsr     (a0)
        addq.l  #4,sp
        and.l   #$ff,d0
        move.l  C_CYC(a4),d5
        rts

; d0.w = address, d1.b = value
slow_write:
        and.l   #$ffff,d0
        tst.b   C_MIRROR(a4)
        beq.s   .call
        move.w  d0,a0
        and.w   #$1080,d0               ; A12=0, A7=0: TIA
        bne.s   .nottia
        tst.b   C_TIADIR(a4)
        beq.s   .nottia
        move.w  a0,d0
        bra     tia_write_direct
.nottia:
        move.w  a0,d0
        and.w   #$1280,d0
        cmp.w   #$0080,d0
        beq.s   .ram
        move.w  a0,d0
        bra.s   .call
.ram:
        move.w  a0,d0
        and.w   #$ff,d0
        move.b  d1,(a6,d0.w)
        addq.l  #1,d5
        rts
.call:
        move.l  d5,C_CYC(a4)
        and.l   #$ff,d1
        move.l  d1,-(sp)
        move.l  d0,-(sp)
        move.l  C_WR(a4),a0
        jsr     (a0)
        addq.l  #8,sp
        move.l  C_CYC(a4),d5
        rts

; zero page write below $80 (d0 = $00-$7F): the TIA in 2600 mode
zp_tia_write:
        tst.b   C_TIADIR(a4)
        beq     slow_write
        tst.b   C_MIRROR(a4)
        beq     slow_write
        ; fall through

; d0.l = TIA address, d1.b = value: straight to tia_write (via C_TIAWR)
tia_write_direct:
        move.l  d5,C_CYC(a4)
        and.l   #$ff,d1
        move.l  d1,-(sp)
        move.l  d0,-(sp)
        move.l  C_TIAWR(a4),a0
        jsr     (a0)
        addq.l  #8,sp
        move.l  C_CYC(a4),d5
        rts

; code fetch outside the current fast page (or in slow mode): find the
; page of the PC; if it is a fast page switch to pointer mode, otherwise
; read through slow_read and stay in slow mode. -> d0.l = byte
fetch_slow:
        GETPC
        move.w  d0,d1
        lsr.w   #8,d1
        move.l  (a5,d1.w*4),a0
        move.l  a0,d1
        beq.s   .slowpage
        move.l  a0,C_PCBIAS(a4)
        lea     (a0,d0.w),a3            ; host pointer of PC (biased map entry)
        move.w  d0,d1
        and.w   #$ff,d1
        neg.w   d1
        add.w   #$100,d1                ; bytes left in this page (1..256)
        lea     (a3,d1.w),a0
        move.l  a0,C_PCEND(a4)
        moveq   #0,d0
        move.b  (a3)+,d0
        addq.l  #1,d5
        rts
.slowpage:
        clr.l   C_PCBIAS(a4)
        clr.l   C_PCEND(a4)
        move.l  d0,a3
        addq.l  #1,a3                   ; PC + 1 (bias 0: a3 is the PC)
        bra     slow_read

; unimplemented opcode: let the C core execute it
fallback:
        subq.l  #1,a3           ; back to the opcode
        subq.l  #1,d5           ; the C core counts the fetch again
        bsr     sync_out
        move.l  C_STEP(a4),a0
        jsr     (a0)
        bsr     sync_in
        bra     loop

; common tail of taken branches: d0 = offset byte, a3 = next instruction
take:
        ext.w   d0
        addq.l  #1,d5
        move.l  C_PCEND(a4),d1
        beq.s   .generic                ; slow mode
        cmp.l   d1,a3
        bhs.s   .generic                ; next instruction is on the next page
        lea     (a3,d0.w),a0
        cmp.l   d1,a0
        bhs.s   .generic                ; target after this page
        sub.l   #256,d1
        cmp.l   d1,a0
        blo.s   .generic                ; target before this page
        move.l  a0,a3                   ; same page: no penalty cycle
        bra     loop
.generic:
        move.w  d0,a0                   ; offset (sign-extended)
        GETPC
        move.l  d0,d1
        add.w   a0,d0
        eor.w   d0,d1
        and.w   #$ff00,d1
        beq.s   .same
        addq.l  #1,d5                   ; page crossed
.same:
        and.l   #$ffff,d0
        SETPC
        bra     loop

; ======================================================================
; opcodes
; ======================================================================

; ---- LDA ----
op_A9:  FETCHB
        OP_LD   d2
        bra     loop
op_A5:  FETCHB
        ZPREAD
        OP_LD   d2
        bra     loop
op_B5:  EA_ZPX
        ZPREAD
        OP_LD   d2
        bra     loop
op_AD:  FETCHW
        READ
        OP_LD   d2
        bra     loop
op_BD:  EA_ABR  d3
        READ
        OP_LD   d2
        bra     loop
op_B9:  EA_ABR  d4
        READ
        OP_LD   d2
        bra     loop
op_A1:  EA_IZX
        READ
        OP_LD   d2
        bra     loop
op_B1:  EA_IZYR
        READ
        OP_LD   d2
        bra     loop

; ---- LDX ----
op_A2:  FETCHB
        OP_LD   d3
        bra     loop
op_A6:  FETCHB
        ZPREAD
        OP_LD   d3
        bra     loop
op_B6:  EA_ZPY
        ZPREAD
        OP_LD   d3
        bra     loop
op_AE:  FETCHW
        READ
        OP_LD   d3
        bra     loop
op_BE:  EA_ABR  d4
        READ
        OP_LD   d3
        bra     loop

; ---- LDY ----
op_A0:  FETCHB
        OP_LD   d4
        bra     loop
op_A4:  FETCHB
        ZPREAD
        OP_LD   d4
        bra     loop
op_B4:  EA_ZPX
        ZPREAD
        OP_LD   d4
        bra     loop
op_AC:  FETCHW
        READ
        OP_LD   d4
        bra     loop
op_BC:  EA_ABR  d3
        READ
        OP_LD   d4
        bra     loop

; ---- STA / STX / STY ----
op_85:  FETCHB
        move.b  d2,d1
        ZPWRITE
        bra     loop
op_95:  EA_ZPX
        move.b  d2,d1
        ZPWRITE
        bra     loop
op_8D:  FETCHW
        move.b  d2,d1
        WRITE
        bra     loop
op_9D:  EA_ABW  d3
        move.b  d2,d1
        WRITE
        bra     loop
op_99:  EA_ABW  d4
        move.b  d2,d1
        WRITE
        bra     loop
op_81:  EA_IZX
        move.b  d2,d1
        WRITE
        bra     loop
op_91:  EA_IZYW
        move.b  d2,d1
        WRITE
        bra     loop
op_86:  FETCHB
        move.b  d3,d1
        ZPWRITE
        bra     loop
op_96:  EA_ZPY
        move.b  d3,d1
        ZPWRITE
        bra     loop
op_8E:  FETCHW
        move.b  d3,d1
        WRITE
        bra     loop
op_84:  FETCHB
        move.b  d4,d1
        ZPWRITE
        bra     loop
op_94:  EA_ZPX
        move.b  d4,d1
        ZPWRITE
        bra     loop
op_8C:  FETCHW
        move.b  d4,d1
        WRITE
        bra     loop

; ---- transfers ----
op_AA:  move.b  d2,d3
        SETNZ   d3
        addq.l  #1,d5
        bra     loop
op_A8:  move.b  d2,d4
        SETNZ   d4
        addq.l  #1,d5
        bra     loop
op_8A:  move.b  d3,d2
        SETNZ   d2
        addq.l  #1,d5
        bra     loop
op_98:  move.b  d4,d2
        SETNZ   d2
        addq.l  #1,d5
        bra     loop
op_BA:  move.b  C_SREG(a4),d3
        SETNZ   d3
        addq.l  #1,d5
        bra     loop
op_9A:  move.b  d3,C_SREG(a4)
        addq.l  #1,d5
        bra     loop

; ---- register inc/dec ----
op_E8:  addq.b  #1,d3
        SETNZ   d3
        addq.l  #1,d5
        bra     loop
op_CA:  subq.b  #1,d3
        SETNZ   d3
        addq.l  #1,d5
        bra     loop
op_C8:  addq.b  #1,d4
        SETNZ   d4
        addq.l  #1,d5
        bra     loop
op_88:  subq.b  #1,d4
        SETNZ   d4
        addq.l  #1,d5
        bra     loop

; ---- memory inc/dec (read, dummy cycle, write) ----
RMW_ZP  macro                   ; \1 = operation on d0
        move.w  d0,C_TMP2(a4)
        ZPREAD
        addq.l  #1,d5
        \1
        move.b  d0,d1
        SETNZ   d1
        move.w  C_TMP2(a4),d0
        ZPWRITE
        bra     loop
        endm

RMW_AB  macro
        move.w  d0,C_TMP2(a4)
        READ
        addq.l  #1,d5
        \1
        move.b  d0,d1
        SETNZ   d1
        moveq   #0,d0
        move.w  C_TMP2(a4),d0
        WRITE
        bra     loop
        endm

op_E6:  FETCHB
        RMW_ZP  <addq.b #1,d0>
op_F6:  EA_ZPX
        RMW_ZP  <addq.b #1,d0>
op_EE:  FETCHW
        RMW_AB  <addq.b #1,d0>
op_FE:  EA_ABW  d3
        RMW_AB  <addq.b #1,d0>
op_C6:  FETCHB
        RMW_ZP  <subq.b #1,d0>
op_D6:  EA_ZPX
        RMW_ZP  <subq.b #1,d0>
op_CE:  FETCHW
        RMW_AB  <subq.b #1,d0>
op_DE:  EA_ABW  d3
        RMW_AB  <subq.b #1,d0>

; ---- shifts ----
op_0A:  add.b   d2,d2
        scs     C_FC(a4)
        SETNZ   d2
        addq.l  #1,d5
        bra     loop
op_4A:  lsr.b   #1,d2
        scs     C_FC(a4)
        SETNZ   d2
        addq.l  #1,d5
        bra     loop
op_2A:  move.b  C_FC(a4),d1
        add.b   d1,d1
        roxl.b  #1,d2
        scs     C_FC(a4)
        SETNZ   d2
        addq.l  #1,d5
        bra     loop
op_6A:  move.b  C_FC(a4),d1
        add.b   d1,d1
        roxr.b  #1,d2
        scs     C_FC(a4)
        SETNZ   d2
        addq.l  #1,d5
        bra     loop

SH_ASL  macro
        add.b   d0,d0
        scs     C_FC(a4)
        endm
SH_LSR  macro
        lsr.b   #1,d0
        scs     C_FC(a4)
        endm
SH_ROL  macro
        move.b  C_FC(a4),d1
        add.b   d1,d1
        roxl.b  #1,d0
        scs     C_FC(a4)
        endm
SH_ROR  macro
        move.b  C_FC(a4),d1
        add.b   d1,d1
        roxr.b  #1,d0
        scs     C_FC(a4)
        endm

op_06:  FETCHB
        RMW_ZP  SH_ASL
op_16:  EA_ZPX
        RMW_ZP  SH_ASL
op_46:  FETCHB
        RMW_ZP  SH_LSR
op_56:  EA_ZPX
        RMW_ZP  SH_LSR
op_26:  FETCHB
        RMW_ZP  SH_ROL
op_36:  EA_ZPX
        RMW_ZP  SH_ROL
op_66:  FETCHB
        RMW_ZP  SH_ROR
op_76:  EA_ZPX
        RMW_ZP  SH_ROR

; ---- logic / arithmetic / compare ----
; \1 = operation macro, \2 = its argument
ALU_IMM macro
        FETCHB
        \1      \2
        bra     loop
        endm
ALU_ZP  macro
        FETCHB
        ZPREAD
        \1      \2
        bra     loop
        endm
ALU_ZPX macro
        EA_ZPX
        ZPREAD
        \1      \2
        bra     loop
        endm
ALU_ABS macro
        FETCHW
        READ
        \1      \2
        bra     loop
        endm
ALU_ABX macro
        EA_ABR  d3
        READ
        \1      \2
        bra     loop
        endm
ALU_ABY macro
        EA_ABR  d4
        READ
        \1      \2
        bra     loop
        endm
ALU_IZY macro
        EA_IZYR
        READ
        \1      \2
        bra     loop
        endm

OP_AND  macro
        and.b   d0,d2
        SETNZ   d2
        endm
OP_ORA  macro
        or.b    d0,d2
        SETNZ   d2
        endm
OP_EOR  macro
        eor.b   d0,d2
        SETNZ   d2
        endm

op_29:  ALU_IMM OP_AND
op_25:  ALU_ZP  OP_AND
op_35:  ALU_ZPX OP_AND
op_2D:  ALU_ABS OP_AND
op_3D:  ALU_ABX OP_AND
op_39:  ALU_ABY OP_AND
op_31:  ALU_IZY OP_AND

op_09:  ALU_IMM OP_ORA
op_05:  ALU_ZP  OP_ORA
op_15:  ALU_ZPX OP_ORA
op_0D:  ALU_ABS OP_ORA
op_1D:  ALU_ABX OP_ORA
op_19:  ALU_ABY OP_ORA
op_11:  ALU_IZY OP_ORA

op_49:  ALU_IMM OP_EOR
op_45:  ALU_ZP  OP_EOR
op_55:  ALU_ZPX OP_EOR
op_4D:  ALU_ABS OP_EOR
op_5D:  ALU_ABX OP_EOR
op_59:  ALU_ABY OP_EOR
op_51:  ALU_IZY OP_EOR

op_C9:  ALU_IMM OP_CMP,d2
op_C5:  ALU_ZP  OP_CMP,d2
op_D5:  ALU_ZPX OP_CMP,d2
op_CD:  ALU_ABS OP_CMP,d2
op_DD:  ALU_ABX OP_CMP,d2
op_D9:  ALU_ABY OP_CMP,d2
op_D1:  ALU_IZY OP_CMP,d2
op_E0:  ALU_IMM OP_CMP,d3
op_E4:  ALU_ZP  OP_CMP,d3
op_EC:  ALU_ABS OP_CMP,d3
op_C0:  ALU_IMM OP_CMP,d4
op_C4:  ALU_ZP  OP_CMP,d4
op_CC:  ALU_ABS OP_CMP,d4

op_24:  ALU_ZP  OP_BIT
op_2C:  ALU_ABS OP_BIT

op_69:  NODEC
        ALU_IMM OP_ADC
op_65:  NODEC
        ALU_ZP  OP_ADC
op_75:  NODEC
        ALU_ZPX OP_ADC
op_6D:  NODEC
        ALU_ABS OP_ADC
op_7D:  NODEC
        ALU_ABX OP_ADC
op_79:  NODEC
        ALU_ABY OP_ADC
op_71:  NODEC
        ALU_IZY OP_ADC

op_E9:  NODEC
        ALU_IMM OP_SBC
op_E5:  NODEC
        ALU_ZP  OP_SBC
op_F5:  NODEC
        ALU_ZPX OP_SBC
op_ED:  NODEC
        ALU_ABS OP_SBC
op_FD:  NODEC
        ALU_ABX OP_SBC
op_F9:  NODEC
        ALU_ABY OP_SBC
op_F1:  NODEC
        ALU_IZY OP_SBC

; ---- branches ----
op_10:  FETCHB                  ; BPL
        tst.b   d6
        bmi     loop
        bra     take
op_30:  FETCHB                  ; BMI
        tst.b   d6
        bpl     loop
        bra     take
op_50:  FETCHB                  ; BVC
        tst.b   C_FV(a4)
        bne     loop
        bra     take
op_70:  FETCHB                  ; BVS
        tst.b   C_FV(a4)
        beq     loop
        bra     take
op_90:  FETCHB                  ; BCC
        tst.b   C_FC(a4)
        bne     loop
        bra     take
op_B0:  FETCHB                  ; BCS
        tst.b   C_FC(a4)
        beq     loop
        bra     take
op_D0:  FETCHB                  ; BNE
        tst.b   d7
        beq     loop
        bra     take
op_F0:  FETCHB                  ; BEQ
        tst.b   d7
        bne     loop
        bra     take

; ---- flags ----
op_18:  clr.b   C_FC(a4)
        addq.l  #1,d5
        bra     loop
op_38:  st      C_FC(a4)
        addq.l  #1,d5
        bra     loop
op_58:  clr.b   C_FI(a4)
        addq.l  #1,d5
        bra     loop
op_78:  st      C_FI(a4)
        addq.l  #1,d5
        bra     loop
op_B8:  clr.b   C_FV(a4)
        addq.l  #1,d5
        bra     loop
op_D8:  clr.b   C_FD(a4)
        addq.l  #1,d5
        bra     loop
op_F8:  st      C_FD(a4)
        addq.l  #1,d5
        bra     loop
op_EA:  addq.l  #1,d5           ; NOP
        bra     loop

; ---- jumps ----
op_4C:  FETCHW                  ; JMP abs
        SETPC
        bra     loop
op_6C:  FETCHW                  ; JMP (ind), with the page wrap bug
        move.w  d0,C_TMP2(a4)
        READ
        move.b  d0,C_TMP+1(a4)
        move.w  C_TMP2(a4),d0
        addq.b  #1,d0
        READ
        move.b  d0,C_TMP(a4)
        moveq   #0,d0
        move.w  C_TMP(a4),d0
        SETPC
        bra     loop
op_20:  FETCHB                  ; JSR: lo, dummy, push PCH, push PCL, hi
        move.b  d0,C_TMP2+1(a4)
        addq.l  #1,d5
        GETPC                           ; address of the high operand byte
        move.w  d0,C_TMP(a4)
        move.b  C_TMP(a4),d1
        PUSH
        move.b  C_TMP+1(a4),d1
        PUSH
        FETCHB
        move.b  d0,C_TMP2(a4)
        moveq   #0,d0
        move.w  C_TMP2(a4),d0
        SETPC
        bra     loop
op_60:  addq.l  #2,d5           ; RTS
        PULL
        move.b  d0,C_TMP2+1(a4)
        PULL
        move.b  d0,C_TMP2(a4)
        moveq   #0,d0
        move.w  C_TMP2(a4),d0
        addq.w  #1,d0
        SETPC
        addq.l  #1,d5
        bra     loop

; ---- stack ----
op_48:  addq.l  #1,d5           ; PHA
        move.b  d2,d1
        PUSH
        bra     loop
op_08:  addq.l  #1,d5           ; PHP
        bsr     packp
        or.b    #$10,d0
        move.b  d0,d1
        PUSH
        bra     loop
op_68:  addq.l  #2,d5           ; PLA
        PULL
        move.b  d0,d2
        SETNZ   d2
        bra     loop
op_28:  addq.l  #2,d5           ; PLP
        PULL
        bsr     unpackp
        bra     loop

; ======================================================================
; opcode table (unlisted opcodes -> fallback to the C core)
; ======================================================================
        cnop    0,4
optable:
        dc.l    fallback,fallback,fallback,fallback,fallback,op_05,op_06,fallback   ; $00
        dc.l    op_08,op_09,op_0A,fallback,fallback,op_0D,fallback,fallback   ; $08
        dc.l    op_10,op_11,fallback,fallback,fallback,op_15,op_16,fallback   ; $10
        dc.l    op_18,op_19,fallback,fallback,fallback,op_1D,fallback,fallback   ; $18
        dc.l    op_20,fallback,fallback,fallback,op_24,op_25,op_26,fallback   ; $20
        dc.l    op_28,op_29,op_2A,fallback,op_2C,op_2D,fallback,fallback   ; $28
        dc.l    op_30,op_31,fallback,fallback,fallback,op_35,op_36,fallback   ; $30
        dc.l    op_38,op_39,fallback,fallback,fallback,op_3D,fallback,fallback   ; $38
        dc.l    fallback,fallback,fallback,fallback,fallback,op_45,op_46,fallback   ; $40
        dc.l    op_48,op_49,op_4A,fallback,op_4C,op_4D,fallback,fallback   ; $48
        dc.l    op_50,op_51,fallback,fallback,fallback,op_55,op_56,fallback   ; $50
        dc.l    op_58,op_59,fallback,fallback,fallback,op_5D,fallback,fallback   ; $58
        dc.l    op_60,fallback,fallback,fallback,fallback,op_65,op_66,fallback   ; $60
        dc.l    op_68,op_69,op_6A,fallback,op_6C,op_6D,fallback,fallback   ; $68
        dc.l    op_70,op_71,fallback,fallback,fallback,op_75,op_76,fallback   ; $70
        dc.l    op_78,op_79,fallback,fallback,fallback,op_7D,fallback,fallback   ; $78
        dc.l    fallback,op_81,fallback,fallback,op_84,op_85,op_86,fallback   ; $80
        dc.l    op_88,fallback,op_8A,fallback,op_8C,op_8D,op_8E,fallback   ; $88
        dc.l    op_90,op_91,fallback,fallback,op_94,op_95,op_96,fallback   ; $90
        dc.l    op_98,op_99,op_9A,fallback,fallback,op_9D,fallback,fallback   ; $98
        dc.l    op_A0,op_A1,op_A2,fallback,op_A4,op_A5,op_A6,fallback   ; $A0
        dc.l    op_A8,op_A9,op_AA,fallback,op_AC,op_AD,op_AE,fallback   ; $A8
        dc.l    op_B0,op_B1,fallback,fallback,op_B4,op_B5,op_B6,fallback   ; $B0
        dc.l    op_B8,op_B9,op_BA,fallback,op_BC,op_BD,op_BE,fallback   ; $B8
        dc.l    op_C0,fallback,fallback,fallback,op_C4,op_C5,op_C6,fallback   ; $C0
        dc.l    op_C8,op_C9,op_CA,fallback,op_CC,op_CD,op_CE,fallback   ; $C8
        dc.l    op_D0,op_D1,fallback,fallback,fallback,op_D5,op_D6,fallback   ; $D0
        dc.l    op_D8,op_D9,fallback,fallback,fallback,op_DD,op_DE,fallback   ; $D8
        dc.l    op_E0,fallback,fallback,fallback,op_E4,op_E5,op_E6,fallback   ; $E0
        dc.l    op_E8,op_E9,op_EA,fallback,op_EC,op_ED,op_EE,fallback   ; $E8
        dc.l    op_F0,op_F1,fallback,fallback,fallback,op_F5,op_F6,fallback   ; $F0
        dc.l    op_F8,op_F9,fallback,fallback,fallback,op_FD,op_FE,fallback   ; $F8

        end
