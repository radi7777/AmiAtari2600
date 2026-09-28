; tia_asm.s - 68020+ version of the TIA renderer render() in src/core/tia.c
;
; void tia_render(u8 *out, int x0, int x1)
;
; Draws pixels [x0, x1) of the current line into out: VBLANK, HMOVE blank,
; playfield/background in 4-pixel blocks, then the objects as stamps with
; the full priority and collision logic. Same results as the C version
; (checked by tests/tia_equiv.c on 68k, see tests/amiga_tests.sh).

        machine 68020

        xdef    _tia_render
        xref    _tia
        xref    _tia_pf_mask            ; u32 [2][160]
        xref    _tia_coll_table         ; u16 [64]
        xref    _tia_zero_mask          ; u8 [320], all 0
        xref    _tia_prio_table         ; u8 [2][64]

        include "tia_offs.i"

C_PF      equ 1                     ; colour index of the playfield

        section "CODE",code

; ---------------------------------------------------------------------
; fill out[d0..d1) with byte d3 (d3 replicated to a longword inside)
; trashes d2, d3, a1
fill:
        cmp.l   d1,d0
        bge.s   .done
        lea     (a0,d0.l),a1
        move.l  d1,d2
        sub.l   d0,d2                   ; count
.head:  move.l  a1,d0
        and.w   #3,d0
        beq.s   .al
        move.b  d3,(a1)+
        subq.l  #1,d2
        bne.s   .head
        rts
.al:    move.b  d3,d0
        lsl.w   #8,d3
        move.b  d0,d3
        move.w  d3,d0
        swap    d3
        move.w  d0,d3
        move.l  d2,d0
        lsr.l   #2,d0
        beq.s   .tail
        subq.w  #1,d0
.long:  move.l  d3,(a1)+
        dbra    d0,.long
.tail:  and.w   #3,d2
        beq.s   .done
        subq.w  #1,d2
.tb:    move.b  d3,(a1)+
        dbra    d2,.tb
.done:  rts

; ---------------------------------------------------------------------
_tia_render:
        movem.l d2-d7/a2-a6,-(sp)
        lea     _tia,a6
        move.l  4+44(sp),a0             ; out
        move.l  8+44(sp),d0             ; x0
        move.l  12+44(sp),d1            ; x1

        btst    #1,T_VBLANK(a6)
        beq.s   .visible
        moveq   #0,d3
        bsr     fill
        bra     .ret

.visible:
        move.l  T_LINE(a6),d2
        tst.l   T_CFV(a6)
        bpl.s   .cfv
        move.l  d2,T_CFV(a6)
.cfv:   move.l  d2,T_CLV(a6)

        tst.b   T_HMB(a6)
        beq.s   .nohmb
        cmp.l   #8,d0
        bge.s   .nohmb
        move.l  d1,d5                   ; x1
        moveq   #8,d4
        cmp.l   d4,d1
        bge.s   .h1
        move.l  d1,d4                   ; e = min(x1, 8)
.h1:    move.l  d4,d1
        moveq   #0,d3
        bsr     fill                    ; HMOVE blank [x0, e)
        move.l  d4,d0
        move.l  d5,d1
        cmp.l   d1,d0
        bge     .ret
.nohmb:
        movem.l d0-d1,-(sp)             ; keep x0/x1 for the stamps
        bsr     playfield
        movem.l (sp)+,d0-d1

        ; ---- objects ----
        tst.b   T_GP0(a6)
        beq.s   .o1
        moveq   #0,d7
        bsr     stamp
.o1:    tst.b   T_GP1(a6)
        beq.s   .o2
        moveq   #1,d7
        bsr     stamp
.o2:    tst.b   T_M0ON(a6)
        beq.s   .o3
        moveq   #2,d7
        bsr     stamp
.o3:    tst.b   T_M1ON(a6)
        beq.s   .o4
        moveq   #3,d7
        bsr     stamp
.o4:    tst.b   T_BLON(a6)
        beq.s   .ret
        moveq   #4,d7
        bsr     stamp
.ret:   movem.l (sp)+,d2-d7/a2-a6
        rts

; ---------------------------------------------------------------------
; playfield + background for out[d0..d1); a0 = out, a6 = tia
; colour of block b = x/4: bit = pf bit b (b < 20), else pf bit b-20
; (normal) or 39-b (reflected); set: col_l[PF] (b < 20) / col_r[PF]
playfield:
        move.l  T_PF(a6),d2
        moveq   #0,d3
        move.b  T_COLUBK(a6),d3
        tst.l   d2
        beq     fill                    ; no playfield: plain fill (tail call)
        moveq   #0,d6
        move.b  T_CTRLPF(a6),d6
        and.w   #1,d6                   ; reflect
        move.l  d1,a5                   ; x1

        ; unaligned head: part of one block
        move.l  d0,d5
        and.w   #3,d5
        beq.s   .blocks
        move.l  d0,d1
        lsr.l   #2,d1
        bsr     blkcol                  ; -> d4.b
        lea     (a0,d0.l),a1
        neg.w   d5
        addq.w  #4,d5                   ; pixels up to the block end
        move.l  a5,d1
        sub.l   d0,d1                   ; pixels left
        cmp.l   d1,d5
        ble.s   .hc
        move.l  d1,d5
.hc:    add.l   d5,d0
        subq.w  #1,d5
.hw:    move.b  d4,(a1)+
        dbra    d5,.hw
        cmp.l   a5,d0
        bge     .pfdone

.blocks:
        ; replicate the three colours: d3 = background, d4 = left, d5 = right
        move.b  d3,d4
        lsl.w   #8,d3
        move.b  d4,d3
        move.w  d3,d4
        swap    d3
        move.w  d4,d3
        moveq   #0,d4
        move.b  T_COLL_L+C_PF(a6),d4
        move.b  d4,d5
        lsl.w   #8,d4
        move.b  d5,d4
        move.w  d4,d5
        swap    d4
        move.w  d5,d4
        moveq   #0,d5
        move.b  T_COLR+C_PF(a6),d5
        move.b  d5,d1
        lsl.w   #8,d5
        move.b  d1,d5
        move.w  d5,d1
        swap    d5
        move.w  d1,d5

        lea     (a0,d0.l),a1            ; output, aligned
        move.l  d0,d7
        lsr.l   #2,d7                   ; first block
        move.l  a5,d1
        lsr.l   #2,d1                   ; first block not fully inside
        cmp.l   d1,d7
        bge     .tailp
        cmp.w   #20,d7
        bge.s   .right
        ; left half: bits d7.. of pf
        move.l  d2,d1
        lsr.l   d7,d1                   ; current bit in bit 0
        move.l  a5,d0
        lsr.l   #2,d0
        cmp.w   #20,d0
        ble.s   .le
        moveq   #20,d0
.le:    sub.w   d7,d0                   ; blocks in the left half
        add.w   d0,d7                   ; block after them
        subq.w  #1,d0
.lb:    lsr.l   #1,d1
        bcs.s   .lon
        move.l  d3,(a1)+
        dbra    d0,.lb
        bra.s   .lend
.lon:   move.l  d4,(a1)+
        dbra    d0,.lb
.lend:  move.l  a5,d1
        lsr.l   #2,d1
        cmp.l   d1,d7
        bge.s   .tailp
.right: ; right half: blocks d7 (>= 20) .. d1-1
        move.l  d1,d0
        sub.w   d7,d0
        subq.w  #1,d0                   ; count - 1
        tst.w   d6
        bne.s   .refl
        move.l  d7,d1
        sub.w   #20,d1
        move.l  d2,d4
        lsr.l   d1,d4
.rb:    lsr.l   #1,d4
        bcs.s   .ron
        move.l  d3,(a1)+
        dbra    d0,.rb
        bra.s   .rend
.ron:   move.l  d5,(a1)+
        dbra    d0,.rb
        bra.s   .rend
.refl:  ; bit 39-b first: shift it to bit 31, then shift left
        move.l  d7,d1
        subq.w  #8,d1                   ; b - 8 = 31 - (39 - b)
        move.l  d2,d4
        lsl.l   d1,d4                   ; bit 39-b now in bit 31
.fb:    lsl.l   #1,d4
        bcs.s   .fon
        move.l  d3,(a1)+
        dbra    d0,.fb
        bra.s   .rend
.fon:   move.l  d5,(a1)+
        dbra    d0,.fb
.rend:  move.l  a5,d7
        lsr.l   #2,d7                   ; last (partial) block
.tailp: ; partial last block: pixels last*4 .. x1-1
        move.l  a5,d5
        and.w   #3,d5
        beq.s   .pfdone
        move.l  d7,d1
        move.b  T_COLUBK(a6),d3
        bsr     blkcol
        lsl.l   #2,d7
        lea     (a0,d7.l),a1
        subq.w  #1,d5
.tw:    move.b  d4,(a1)+
        dbra    d5,.tw
.pfdone:
        rts

; colour of playfield block d1 -> d4.b (d2 = pf, d6 = reflect, d3.b = background)
blkcol:
        cmp.w   #20,d1
        blt.s   .l
        sub.w   #20,d1
        tst.w   d6
        beq.s   .r
        neg.w   d1
        add.w   #19,d1                  ; 39 - b = 19 - (b - 20)
.r:     btst    d1,d2
        beq.s   .bk
        move.b  T_COLR+C_PF(a6),d4
        rts
.l:     btst    d1,d2
        beq.s   .bk
        move.b  T_COLL_L+C_PF(a6),d4
        rts
.bk:    move.b  d3,d4
        rts

; ---------------------------------------------------------------------
; stamp object d7 (0..4) into out[d0..d1): evaluate its runs
stamp:
        movem.l d0-d1,-(sp)
        move.l  d0,d4                   ; x0
        move.l  d1,d5                   ; x1
        move.l  T_RUNS(a6,d7.w*4),a2    ; runs
        moveq   #0,d6
        move.b  T_NRUNS(a6,d7.w),d6
        moveq   #0,d3
        move.b  T_POS(a6,d7.w),d3       ; pos
        bra.s   .next
.run:   moveq   #0,d0
        move.b  (a2)+,d0
        add.w   d3,d0                   ; s = pos + off
        cmp.w   #160,d0
        blt.s   .s1
        sub.w   #160,d0
.s1:    moveq   #0,d1
        move.b  (a2)+,d1
        add.w   d0,d1                   ; e = s + len
        cmp.w   #160,d1
        ble.s   .nowrap
        ; wraps: [max(s,x0), x1) and [x0, min(e-160,x1))
        movem.l d0-d1,-(sp)
        cmp.l   d4,d0
        bge.s   .w1
        move.l  d4,d0
.w1:    move.l  d5,d1
        cmp.l   d1,d0
        bge.s   .w2
        bsr     eval
.w2:    movem.l (sp)+,d0-d1
        sub.w   #160,d1
        cmp.l   d5,d1
        ble.s   .w3
        move.l  d5,d1
.w3:    move.l  d4,d0
        cmp.l   d1,d0
        bge.s   .next
        bsr     eval
        bra.s   .next
.nowrap:
        cmp.l   d4,d0
        bge.s   .c1
        move.l  d4,d0
.c1:    cmp.l   d5,d1
        ble.s   .c2
        move.l  d5,d1
.c2:    cmp.l   d1,d0
        bge.s   .next
        bsr     eval
.next:  dbra    d6,.run2
        movem.l (sp)+,d0-d1
        rts
.run2:  bra.s   .run

; ---------------------------------------------------------------------
; full priority + collision evaluation of pixels [d0, d1)
; a0 = out; keeps everything but d0 (the Tia is addressed absolutely here
; to have a register for every object mask)
eval:
        movem.l d2-d7/a1-a6,-(sp)
        moveq   #0,d7
        lea     _tia_pf_mask,a1
        btst    #0,_tia+T_CTRLPF
        beq.s   .e0
        lea     160*4(a1),a1            ; reflected playfield
.e0:    move.l  _tia+T_P0M,a2
        move.l  _tia+T_P1M,a3
        lea     _tia_zero_mask,a4
        move.l  a4,a5
        move.l  a4,a6
        tst.b   _tia+T_M0ON
        beq.s   .e1
        move.l  _tia+T_M0M,a4
.e1:    tst.b   _tia+T_M1ON
        beq.s   .e2
        move.l  _tia+T_M1M,a5
.e2:    tst.b   _tia+T_BLON
        beq.s   .e3
        move.l  _tia+T_BLM,a6
.e3:    move.l  _tia+T_PF,d2
        move.b  _tia+T_GP0,d5
        move.b  _tia+T_GP1,d6
        btst    #2,_tia+T_CTRLPF        ; playfield priority: second table
        beq.s   .px
        moveq   #64,d7
.px:    ; o bits: PF 1, BL 2, M0 4, M1 8, P0 16, P1 32
        moveq   #0,d4
        move.l  (a1,d0.l*4),d3
        and.l   d2,d3
        beq.s   .n1
        moveq   #1,d4
.n1:    move.b  (a2,d0.l),d3
        and.b   d5,d3
        beq.s   .n2
        or.b    #16,d4
.n2:    move.b  (a3,d0.l),d3
        and.b   d6,d3
        beq.s   .n3
        or.b    #32,d4
.n3:    tst.b   (a4,d0.l)
        beq.s   .n4
        addq.b  #4,d4
.n4:    tst.b   (a5,d0.l)
        beq.s   .n5
        addq.b  #8,d4
.n5:    tst.b   (a6,d0.l)
        beq.s   .n6
        addq.b  #2,d4
.n6:    move.w  (_tia_coll_table,d4.w*2),d3
        or.w    d3,_tia+T_COLL
        add.w   d7,d4
        moveq   #0,d3
        move.b  (_tia_prio_table,d4.w),d3       ; colour index 0..4
        cmp.w   #80,d0
        bge.s   .rc
        move.b  (_tia+T_COLL_L,d3.w),(a0,d0.l)
        bra.s   .nx
.rc:    move.b  (_tia+T_COLR,d3.w),(a0,d0.l)
.nx:    addq.l  #1,d0
        cmp.l   d1,d0
        blt.s   .px
        movem.l (sp)+,d2-d7/a1-a6
        rts

