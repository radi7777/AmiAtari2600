; cpu_paths.asm - cycle exact CPU paths that the 68k assembler core treats
; specially (8K F8 cartridge):
;   P0: delay loop whose taken branch crosses a page (+1 cycle each)
;       -> RESP0 lands at pixel 51 (pixel 39 without the penalty)
;   P1: the same kind of loop executed from RIOT RAM (slow page code)
;       -> RESP1 lands at pixel 42
;   background: code in page $1Fxx switches to bank 0, which continues at
;       the next address and sets the colour $84 (blue); bank 1 would set
;       $0E (white) there.
        include "vcs.inc"
RAMCODE = $90

        ; ---------------- bank 0 (file offset 0) ----------------
        org $E000
        rorg $F000
        .byte 0
        org $EFA0
        rorg $FFA0
        lda $1FF8               ; (not reached in bank 0)
        lda #$84                ; $FFA3: reached right after the switch
        sta COLUBK
        lda $1FF9               ; back to bank 1, continues at $FFAA
        org $EF00
        rorg $FF00
stub0:  sei
        cld
        ldx #$FF
        txs
        lda $1FF9
        jmp main
        org $EFFC
        rorg $FFFC
        .word stub0
        .word stub0

        ; ---------------- bank 1 (file offset 4096) ----------------
        org $F000
        rorg $F000
main:   ldx #0
        lda #0
clr:    sta 0,x
        dex
        bne clr
        ldx #ramend-ramsrc
copy:   lda ramsrc-1,x
        sta RAMCODE-1,x
        dex
        bne copy
        lda #$44
        sta COLUP0
        lda #$C8
        sta COLUP1
frame:  lda #2
        sta VBLANK
        sta VSYNC
        sta WSYNC
        sta WSYNC
        sta WSYNC
        lda #0
        sta VSYNC
        ; P0: branch crossing a page
        sta WSYNC
        jmp p0go
p0back: ; P1: loop in RAM
        sta WSYNC
        jsr RAMCODE
        ; background from bank 0
        jsr $FFA0
        ldx #35
vb:     sta WSYNC
        dex
        bne vb
        lda #0
        sta VBLANK
        lda #$80
        sta GRP0
        sta GRP1
        ldx #192
kern:   sta WSYNC
        dex
        bne kern
        lda #2
        sta VBLANK
        ldx #30
os:     sta WSYNC
        dex
        bne os
        jmp frame

ramsrc: ldx #5                  ; copied to RAM: 2
rl:     dex                     ; 4 x 5 + 4
        .byte $D0,$FD           ; bne rl
        sta RESP1               ; write on cycle 6+2+24+3 = 35 -> pixel 42
        rts
ramend:

        org $F1FA
        rorg $F1FA
p0go:   ldx #5                  ; 3 (jmp) + 2
        nop                     ; 2
p0l:    dex                     ; $F1FD: 4 x (2 + 4) + 2 + 2
        bne p0l                 ; next instruction at $F200: taken branch crosses
        sta RESP0               ; write on cycle 3+2+2+28+3 = 38 -> pixel 51
        jmp p0back

        org $FFA0
        rorg $FFA0
        lda $1FF8               ; switch to bank 0, which continues at $FFA3
        lda #$0E                ; (bank 1: must not run)
        sta COLUBK
        nop
        nop
        nop
        rts                     ; $FFAA
        org $FF00
        rorg $FF00
stub1:  sei
        cld
        ldx #$FF
        txs
        lda $1FF9
        jmp main
        org $FFFC
        rorg $FFFC
        .word stub1
        .word stub1
