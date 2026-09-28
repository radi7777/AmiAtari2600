; vidconv_asm.s - 68020+ versions of vc_convert_line and vc_same_line
; (see vidconv.h / vidconv.c for the algorithm; results are identical to
; the C versions, which remain the reference and the fallback for lines
; with more than 16 colours).
;
; int vc_convert_line(const u8 *line, int bank, u32 *moves, u32 *planes)
; int vc_same_line(const u32 *a, const u32 *b)

        machine 68020

        xdef    _vc_convert_line
        xdef    _vc_same_line
        xref    _vc_rgb12
        xref    _vc_slotmap
        xref    _vc_c2p_w
        xref    _vc_c2p_w4
        xref    _vc_convert_line_c

        section "CODE",code

; ---------------------------------------------------------------------
_vc_same_line:
        move.l  4(sp),a0
        move.l  8(sp),a1
        moveq   #9,d1
.l:     cmpm.l  (a0)+,(a1)+
        bne.s   .no
        cmpm.l  (a0)+,(a1)+
        bne.s   .no
        cmpm.l  (a0)+,(a1)+
        bne.s   .no
        cmpm.l  (a0)+,(a1)+
        bne.s   .no
        dbra    d1,.l
        moveq   #1,d0
        rts
.no:    moveq   #0,d0
        rts

; ---------------------------------------------------------------------
SAVED   equ     11*4            ; d2-d7/a2-a6
SLOTS   equ     160             ; slot buffer on the stack
ARG     equ     SAVED+SLOTS+4
A_LINE  equ     ARG
A_BANK  equ     ARG+4
A_MOVES equ     ARG+8
A_PLAN  equ     ARG+12

; map one colour byte (top byte of d0 after rol) to a register -> d5
MAPB    macro
        rol.l   #8,d0
        moveq   #0,d1
        move.b  d0,d1
        move.b  (a2,d1.w),d2
        bpl.s   .ok\@
        bsr     newcol
.ok\@:
        lsl.l   #8,d5
        move.b  d2,d5
        endm

_vc_convert_line:
        movem.l d2-d7/a2-a6,-(sp)
        lea     -SLOTS(sp),sp
        move.l  A_LINE(sp),a0
        move.l  sp,a1                   ; slots
        lea     _vc_slotmap,a2
        move.l  A_MOVES(sp),a3
        lea     lcols,a4
        lea     _vc_rgb12,a6
        move.l  A_BANK(sp),d4
        lsl.l   #5,d4                   ; bank * 16 registers * 2
        add.l   #$180,d4
        swap    d4                      ; register base << 16
        clr.w   d4
        moveq   #1,d3                   ; registers used (0 = black)
        moveq   #0,d5                   ; registers of the previous 4 pixels
        moveq   #0,d6                   ; previous 4 pixels (black)
        moveq   #39,d7
.lp:    move.l  (a0)+,d0
        cmp.l   d6,d0
        bne.s   .diff
        move.l  d5,(a1)+
        dbra    d7,.lp
        bra.s   .mapped
.diff:  move.l  d0,d6
        MAPB
        MAPB
        MAPB
        MAPB
        move.l  d5,(a1)+
        dbra    d7,.lp

.mapped:
        ; forget this line's colours again
        moveq   #1,d1
        moveq   #0,d0
        bra.s   .rs
.rsl:   move.b  (a4,d1.w),d0
        st      (a2,d0.w)
        addq.w  #1,d1
.rs:    cmp.w   d3,d1
        blt.s   .rsl
        move.l  d3,a4
        subq.l  #1,a4                   ; result: number of moves

        ; ---- c2p: 16 pixels -> one longword per plane ----
        move.l  sp,a0
        lea     SLOTS(sp),a5            ; end of slots
        move.l  A_PLAN(sp),a1
        lea     _vc_c2p_w,a2
        lea     _vc_c2p_w4,a3
        move.l  #$FF00FF00,d2
        move.l  #$00FF00FF,d3
.c2p:   move.w  (a0)+,d0
        move.l  (a3,d0.w*4),d4
        move.w  (a0)+,d0
        or.l    (a2,d0.w*4),d4          ; v0
        move.w  (a0)+,d0
        move.l  (a3,d0.w*4),d5
        move.w  (a0)+,d0
        or.l    (a2,d0.w*4),d5          ; v1
        move.w  (a0)+,d0
        move.l  (a3,d0.w*4),d6
        move.w  (a0)+,d0
        or.l    (a2,d0.w*4),d6          ; v2
        move.w  (a0)+,d0
        move.l  (a3,d0.w*4),d1
        move.w  (a0)+,d0
        or.l    (a2,d0.w*4),d1          ; v3
        ; 4x4 byte transpose
        move.l  d4,d0
        and.l   d2,d0
        move.l  d5,d7
        lsr.l   #8,d7
        and.l   d3,d7
        or.l    d7,d0                   ; t0 = a0 b0 a2 b2
        lsl.l   #8,d4
        and.l   d2,d4
        and.l   d3,d5
        or.l    d5,d4                   ; t1 = a1 b1 a3 b3
        move.l  d6,d5
        and.l   d2,d5
        move.l  d1,d7
        lsr.l   #8,d7
        and.l   d3,d7
        or.l    d7,d5                   ; t2 = c0 d0 c2 d2
        lsl.l   #8,d6
        and.l   d2,d6
        and.l   d3,d1
        or.l    d1,d6                   ; t3 = c1 d1 c3 d3
        swap    d5
        move.l  d0,d7
        move.w  d5,d7                   ; t0.hi : t2.hi
        move.l  d7,(a1)                 ; plane 1
        swap    d5
        swap    d0
        move.w  d5,d0                   ; t0.lo : t2.lo
        move.l  d0,80(a1)               ; plane 3
        swap    d6
        move.l  d4,d7
        move.w  d6,d7                   ; t1.hi : t3.hi
        move.l  d7,40(a1)               ; plane 2
        swap    d6
        swap    d4
        move.w  d6,d4                   ; t1.lo : t3.lo
        move.l  d4,120(a1)              ; plane 4
        addq.l  #4,a1
        cmp.l   a5,a0
        blo.w   .c2p

        move.l  a4,d0
        lea     SLOTS(sp),sp
        movem.l (sp)+,d2-d7/a2-a6
        rts

; new colour d1 (byte, even) -> register d2; assigns the next free one and
; writes its copper move. More than 16 colours: redo the line in C.
newcol:
        cmp.w   #16,d3
        beq.s   .ovf
        move.b  d3,(a2,d1.w)
        move.b  d1,(a4,d3.w)
        move.l  d3,d2
        swap    d2
        add.l   d2,d2                   ; register offset n * 2, << 16
        add.l   d4,d2
        move.w  (a6,d1.w),d2            ; rgb12[colour >> 1] (colour is even)
        move.l  d2,(a3)+
        move.l  d3,d2
        addq.w  #1,d3
        rts
.ovf:   addq.l  #4,sp                   ; drop the return address
        ; undo the register assignments of this line
        moveq   #1,d1
        moveq   #0,d0
.ul:    move.b  (a4,d1.w),d0
        st      (a2,d0.w)
        addq.w  #1,d1
        cmp.w   #16,d1
        blt.s   .ul
        move.l  A_PLAN(sp),-(sp)
        move.l  A_MOVES+4(sp),-(sp)
        move.l  A_BANK+8(sp),-(sp)
        move.l  A_LINE+12(sp),-(sp)
        jsr     _vc_convert_line_c
        lea     16(sp),sp
        lea     SLOTS(sp),sp
        movem.l (sp)+,d2-d7/a2-a6
        rts

        section "BSS",bss
lcols:  ds.b    16                      ; colour byte of each register
