; tiawr_asm.s - TIA register writes from the 68k 6507 core, see below.
; Needs the assembler CPU core (actx) and tia_asm.s (tia_render).

        machine 68020

        include "tia_offs.i"

        xref    _tia
        xref    _tia_render
        xref    _tia_hmove_disp
        xref    _tia_upd_obj
        xref    _tia_rep4
        xref    _tia_memo

        section "CODE",code

; =====================================================================
; TIA register writes from the 68k 6507 core (AsmCpu.tiawr):
;   void tia_write_asm(u32 addr, u32 val)
; Same bookkeeping as asm_tia_write() in atari.c. The frequent registers
; (GRPx, ENAxx, colours, PFx, WSYNC) are handled here, including the
; catch-up of the beam; everything else goes to the C tia_write().

        xdef    _tia_write_asm
        xref    _tia_write
        xref    _tia_end_line
        xref    _tia_reverse_bits
        xref    _tia_scratch_line32
        xref    _a26_cycles
        xref    _a26_databus
        xref    _a26_stop
        xref    _actx

_tia_write_asm:
        movem.l d2-d7/a2-a6,-(sp)
        lea     _tia,a6
        move.l  _actx+C_CYC,d7
        addq.l  #1,d7                   ; the bus cycle of this write
        move.l  d7,_a26_cycles
        move.l  4+44+4(sp),d5           ; val
        move.b  d5,_a26_databus
        mulu.l  #3,d7                   ; cc
        move.l  4+44(sp),d0
        and.w   #$3F,d0
        move.w  .jt(pc,d0.w*2),d0
        jmp     .jt(pc,d0.w)
.jt:    dc.w    .c-.jt,.c-.jt,.wsync-.jt,.c-.jt,.c-.jt,.c-.jt,.colp0-.jt,.colp1-.jt
        dc.w    .colpf-.jt,.colbk-.jt,.c-.jt,.c-.jt,.c-.jt,.pf0-.jt,.pf1-.jt,.pf2-.jt
        dc.w    .c-.jt,.c-.jt,.c-.jt,.c-.jt,.c-.jt,.c-.jt,.c-.jt,.c-.jt
        dc.w    .c-.jt,.c-.jt,.c-.jt,.grp0-.jt,.grp1-.jt,.enam0-.jt,.enam1-.jt,.enabl-.jt
        dc.w    .c-.jt,.c-.jt,.c-.jt,.c-.jt,.c-.jt,.c-.jt,.c-.jt,.c-.jt
        dc.w    .c-.jt,.c-.jt,.hmove-.jt,.c-.jt,.c-.jt,.c-.jt,.c-.jt,.c-.jt
        dc.w    .c-.jt,.c-.jt,.c-.jt,.c-.jt,.c-.jt,.c-.jt,.c-.jt,.c-.jt
        dc.w    .c-.jt,.c-.jt,.c-.jt,.c-.jt,.c-.jt,.c-.jt,.c-.jt,.c-.jt

.hmove: ; HMOVE (see tia.c): locked objects are handled in C
        tst.b   T_HMLOCK(a6)
        bne     .c
        tst.b   T_HMLOCK+1(a6)
        bne     .c
        tst.b   T_HMLOCK+2(a6)
        bne     .c
        tst.b   T_HMLOCK+3(a6)
        bne     .c
        tst.b   T_HMLOCK+4(a6)
        bne     .c
        move.l  d7,d0
        bsr     update_to
        move.l  d7,d0
        sub.l   T_LSC(a6),d0            ; hpos
.hp0:   bpl.s   .hp1
        add.l   #LINE_CC,d0
        bra.s   .hp0
.hp1:   cmp.l   #LINE_CC,d0
        blt.s   .hp2
        sub.l   #LINE_CC,d0
        bra.s   .hp1
.hp2:   move.l  d7,d1
        sub.l   d0,d1
        move.l  d1,T_HMLCC(a6)          ; start of this line
        divu.w  #3,d0
        and.l   #$FFFF,d0               ; w = CPU cycle in the line
        cmp.w   #75,d0
        bls.s   .hp3
        moveq   #75,d0
.hp3:   move.b  d0,T_HMW(a6)
        lea     _tia_hmove_disp,a0
        add.l   d0,a0                   ; &hmove_disp[0][w]
        moveq   #4,d2
        moveq   #0,d3
.hv:    moveq   #0,d1
        move.b  T_HMP0(a6,d3.w),d1
        lsr.b   #4,d1
        move.b  d1,d4
        eor.b   #8,d4
        move.b  d4,T_HMV(a6,d3.w)       ; extra clocks due
        mulu.w  #76,d1
        move.b  (a0,d1.l),T_HMDISP(a6,d3.w)
        addq.w  #1,d3
        dbra    d2,.hv
        cmp.w   #20,d0
        bhi.s   .hb
        move.b  #1,T_HMB(a6)            ; HMOVE blank on this line
.hb:    cmp.w   #54,d0
        bhs.s   .hlate
        ; move now: only objects with a displacement
        moveq   #0,d3
.ha:    move.b  T_HMDISP(a6,d3.w),d1
        beq.s   .hn
        ext.w   d1
        moveq   #0,d4
        move.b  T_POS(a6,d3.w),d4
        add.w   d1,d4
        bpl.s   .hq
        add.w   #160,d4
.hq:    cmp.w   #160,d4
        blt.s   .hr
        sub.w   #160,d4
.hr:    move.b  d4,T_POS(a6,d3.w)
        move.l  d3,-(sp)
        jsr     _tia_upd_obj
        move.l  (sp)+,d3
.hn:    addq.w  #1,d3
        cmp.w   #5,d3
        blt.s   .ha
        clr.b   T_HMPEND(a6)
        bra     .done
.hlate: move.b  #1,T_HMPEND(a6)         ; applied at the end of the line
        bra     .done

.c:     ; everything else: C. Catching up to the write cycle first is
        ; always correct (the C code draws exactly the same pixels) and
        ; cheaper here.
        move.l  d7,d0
        bsr     update_to
        move.l  d5,-(sp)
        move.l  4+44+4(sp),-(sp)
        jsr     _tia_write
        addq.l  #8,sp
        bra     .done

.wsync: move.l  d7,d0
        bsr     update_to
        move.l  T_LAST(a6),d0
        sub.l   T_LSC(a6),d0            ; pos in the line
        ble     .done
        cmp.l   #LINE_CC,d0
        bge     .done
        neg.l   d0
        add.l   #LINE_CC,d0
        divu.w  #3,d0
        and.l   #$FFFF,d0
        add.l   d0,_a26_cycles
        bra     .done

.colp0: lea     T_COLUP0(a6),a1
        bra.s   .col
.colp1: lea     T_COLUP1(a6),a1
        bra.s   .col
.colpf: lea     T_COLUPF(a6),a1
        bra.s   .col
.colbk: lea     T_COLUBK(a6),a1
.col:   and.b   #$FE,d5
        cmp.b   (a1),d5
        beq     .done                   ; unchanged: nothing to draw
        move.l  d7,d0
        bsr     update_to
        move.b  d5,(a1)
        bsr     upd_colors
        bra     .done

.pf0:   lea     T_PF0(a6),a1
        bra.s   .pf
.pf1:   lea     T_PF1(a6),a1
        bra.s   .pf
.pf2:   lea     T_PF2(a6),a1
.pf:    ; delay 4, 5, 2, 3 colour clocks by (hpos / 3) & 3
        move.l  d7,d0
        sub.l   T_LSC(a6),d0
.h0:    bpl.s   .h1
        add.l   #LINE_CC,d0
        bra.s   .h0
.h1:    cmp.l   #LINE_CC,d0
        blt.s   .h2
        sub.l   #LINE_CC,d0
        bra.s   .h1
.h2:    divu.w  #3,d0
        and.w   #3,d0
        moveq   #0,d1
        move.b  .pfd(pc,d0.w),d1
        move.l  d7,d0
        add.l   d1,d0
        bsr     update_to
        move.b  d5,(a1)
        ; pf = pf0 >> 4 | reverse(pf1) << 4 | pf2 << 12
        moveq   #0,d0
        move.b  T_PF0(a6),d0
        lsr.b   #4,d0
        moveq   #0,d1
        move.b  T_PF1(a6),d1
        lea     _tia_reverse_bits,a0
        move.b  (a0,d1.w),d1
        lsl.l   #4,d1
        or.l    d1,d0
        moveq   #0,d1
        move.b  T_PF2(a6),d1
        moveq   #12,d2
        lsl.l   d2,d1
        or.l    d1,d0
        move.l  d0,T_PF(a6)
        bra     .done
.pfd:   dc.b    4,5,2,3

.grp0:  move.l  d7,d0
        addq.l  #1,d0
        bsr     update_to
        move.b  d5,T_GRP0N(a6)
        move.b  T_GRP1N(a6),T_GRP1O(a6)
        bsr     upd_g
        bra     .done

.grp1:  move.l  d7,d0
        addq.l  #1,d0
        bsr     update_to
        move.b  d5,T_GRP1N(a6)
        move.b  T_GRP0N(a6),T_GRP0O(a6)
        move.b  T_ENABLN(a6),T_ENABLO(a6)
        bsr     upd_g
        bsr     upd_blon
        bra     .done

.enam0: lea     T_ENAM0(a6),a1
        lea     T_RESMP0(a6),a2
        lea     T_M0ON(a6),a3
        bra.s   .enam
.enam1: lea     T_ENAM1(a6),a1
        lea     T_RESMP1(a6),a2
        lea     T_M1ON(a6),a3
.enam:  lsr.b   #1,d5
        and.b   #1,d5
        cmp.b   (a1),d5
        beq.s   .done
        move.l  d7,d0
        addq.l  #1,d0
        bsr     update_to
        move.b  d5,(a1)
        tst.b   (a2)
        beq.s   .en1
        moveq   #0,d5
.en1:   move.b  d5,(a3)                 ; on = enam && !resmp
        bra.s   .done

.enabl: lsr.b   #1,d5
        and.b   #1,d5
        cmp.b   T_ENABLN(a6),d5
        beq.s   .done
        move.l  d7,d0
        addq.l  #1,d0
        bsr     update_to
        move.b  d5,T_ENABLN(a6)
        bsr     upd_blon

.done:  move.l  _a26_cycles,d0
        move.l  d0,_actx+C_CYC
        tst.l   _a26_stop
        beq.s   .ret
        move.l  d0,_actx+C_TARGET
.ret:   movem.l (sp)+,d2-d7/a2-a6
        rts

; gp0/gp1 from GRPx (VDEL: old value) and REFPx
upd_g:  lea     _tia_reverse_bits,a0
        moveq   #0,d0
        move.b  T_GRP0N(a6),d0
        tst.b   T_VDELP0(a6)
        beq.s   .g0
        move.b  T_GRP0O(a6),d0
.g0:    tst.b   T_REFP0(a6)
        beq.s   .g1
        move.b  (a0,d0.w),d0
.g1:    move.b  d0,T_GP0(a6)
        moveq   #0,d0
        move.b  T_GRP1N(a6),d0
        tst.b   T_VDELP1(a6)
        beq.s   .g2
        move.b  T_GRP1O(a6),d0
.g2:    tst.b   T_REFP1(a6)
        beq.s   .g3
        move.b  (a0,d0.w),d0
.g3:    move.b  d0,T_GP1(a6)
        rts

upd_blon:
        move.b  T_ENABLN(a6),d0
        tst.b   T_VDELBL(a6)
        beq.s   .b
        move.b  T_ENABLO(a6),d0
.b:     move.b  d0,T_BLON(a6)
        rts

; colour per object index (upd_colors in tia.c): BK 0, PF 1, BL 2, P0 3, P1 4
upd_colors:
        move.b  T_COLUBK(a6),d0
        move.b  d0,T_COLL_L+0(a6)
        move.b  d0,T_COLR+0(a6)
        move.b  T_COLUPF(a6),d0
        move.b  d0,T_COLL_L+2(a6)
        move.b  d0,T_COLR+2(a6)
        move.b  d0,T_COLL_L+1(a6)
        move.b  d0,T_COLR+1(a6)
        move.b  T_COLUP0(a6),d1
        move.b  d1,T_COLL_L+3(a6)
        move.b  d1,T_COLR+3(a6)
        move.b  T_COLUP1(a6),d2
        move.b  d2,T_COLL_L+4(a6)
        move.b  d2,T_COLR+4(a6)
        move.b  T_CTRLPF(a6),d0
        and.b   #6,d0
        cmp.b   #2,d0
        bne.s   .ns
        move.b  d1,T_COLL_L+1(a6)       ; score mode: PF in the player colours
        move.b  d2,T_COLR+1(a6)
.ns:    ; replicated for the playfield blocks (tia_rep4)
        moveq   #0,d0
        move.b  T_COLUBK(a6),d0
        mulu.l  #$01010101,d0
        move.l  d0,_tia_rep4
        moveq   #0,d0
        move.b  T_COLL_L+1(a6),d0
        mulu.l  #$01010101,d0
        move.l  d0,_tia_rep4+4
        moveq   #0,d0
        move.b  T_COLR+1(a6),d0
        mulu.l  #$01010101,d0
        move.l  d0,_tia_rep4+8
        rts

; render up to colour clock d0 (update_to in tia.c); keeps d5, d7, a1-a3
update_to:
        movem.l d5/a1-a3,-(sp)
        move.l  d0,d6                   ; target
.lp:    move.l  d6,d0
        sub.l   T_LAST(a6),d0
        ble.s   .dn
        move.l  T_LAST(a6),d2
        sub.l   T_LSC(a6),d2            ; pos
        move.l  d6,d3
        sub.l   T_LSC(a6),d3            ; end
        cmp.l   #LINE_CC,d3
        ble.s   .e
        move.l  #LINE_CC,d3
.e:     cmp.l   #HBLANK,d3
        ble.s   .nr
        move.l  d3,d1
        sub.l   #HBLANK,d1              ; x1
        move.l  d2,d0
        sub.l   #HBLANK,d0              ; x0
        bpl.s   .x
        moveq   #0,d0
.x:     move.l  T_LINE(a6),d4
        cmp.l   #FB_LINES,d4
        bhs.s   .scr
        move.l  T_FB(a6),a0
        mulu.w  #160,d4
        add.l   d4,a0
        bra.s   .out
.scr:   lea     _tia_scratch_line32,a0
.out:   bsr     render_memo
.nr:    move.l  d3,d0
        sub.l   d2,d0
        add.l   d0,T_LAST(a6)
        cmp.l   #LINE_CC,d3
        bne.s   .lp
        jsr     _tia_end_line
        bra.s   .lp
.dn:    movem.l (sp)+,d5/a1-a3
        rts

; ---------------------------------------------------------------------
; render(): the segment memo of tia.c in front of tia_render
; a0 = out, d0 = x0, d1 = x1; keeps d2-d7/a1-a6
MEMO_SEGS equ 16
MEMO_SIZE equ 88                    ; 20 longwords, x0, x1, collisions, misses

render_memo:
        movem.l d2-d4/a1-a3,-(sp)
        moveq   #0,d2
        move.b  T_SEG(a6),d2
        cmp.w   #MEMO_SEGS,d2
        bhs     .plain
        move.l  T_LINE(a6),d3
        cmp.l   #FB_LINES,d3
        bhs     .plain
        addq.b  #1,T_SEG(a6)
        lsl.l   #4,d3
        add.l   d2,d3
        mulu.w  #MEMO_SIZE,d3
        lea     _tia_memo,a1
        add.l   d3,a1                   ; entry of (line, segment)
        cmp.b   80(a1),d0
        bne.s   .miss
        cmp.b   81(a1),d1
        bne.s   .miss
        lea     T_PF(a6),a2
        move.l  a1,a3
        moveq   #4,d4
.cmp:   cmpm.l  (a2)+,(a3)+
        bne.s   .miss
        cmpm.l  (a2)+,(a3)+
        bne.s   .miss
        cmpm.l  (a2)+,(a3)+
        bne.s   .miss
        cmpm.l  (a2)+,(a3)+
        bne.s   .miss
        dbra    d4,.cmp
        ; hit: the pixels are still there, add the collisions
        clr.b   84(a1)
        move.w  82(a1),d2
        or.w    d2,T_COLL(a6)
        btst    #1,T_VBLANK(a6)
        bne.s   .done
        move.l  T_LINE(a6),d2
        tst.l   T_CFV(a6)
        bpl.s   .cfv
        move.l  d2,T_CFV(a6)
.cfv:   move.l  d2,T_CLV(a6)
        bra.s   .done
.miss:  ; keeps changing: after two misses only every 16th stores it
        addq.b  #1,84(a1)
        bcc.s   .m1
        st      84(a1)                  ; saturate at 255
.m1:    move.b  84(a1),d2
        cmp.b   #2,d2
        bls.s   .store
        and.b   #15,d2
        beq.s   .store
        st      80(a1)                  ; drawn without the memo: invalid
        bra     .plain
.store: lea     T_PF(a6),a2
        move.l  a1,a3
        moveq   #19,d4
.cp:    move.l  (a2)+,(a3)+
        dbra    d4,.cp
        move.b  d0,80(a1)
        move.b  d1,81(a1)
        move.w  T_COLL(a6),d4
        clr.w   T_COLL(a6)
        move.l  a1,a3                   ; kept by tia_render
        move.l  d1,-(sp)
        move.l  d0,-(sp)
        move.l  a0,-(sp)
        jsr     _tia_render
        lea     12(sp),sp
        move.w  T_COLL(a6),82(a3)
        or.w    d4,T_COLL(a6)
.done:  movem.l (sp)+,d2-d4/a1-a3
        rts
.plain: move.l  d1,-(sp)
        move.l  d0,-(sp)
        move.l  a0,-(sp)
        jsr     _tia_render
        lea     12(sp),sp
        movem.l (sp)+,d2-d4/a1-a3
        rts
