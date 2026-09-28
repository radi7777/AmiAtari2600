; bars_ntsc.asm - NTSC timing (3 + 37 + 192 + 30 lines), playfield,
; player 0 position and a pure audio tone.
        include "vcs.inc"
        org $F000
reset:  sei
        cld
        ldx #$FF
        txs
        lda #0
clr:    sta 0,x
        dex
        bne clr
        lda #4
        sta AUDC0
        lda #10
        sta AUDF0
        lda #15
        sta AUDV0
frame:  lda #2
        sta VBLANK
        sta VSYNC
        sta WSYNC
        sta WSYNC
        sta WSYNC
        lda #0
        sta VSYNC
        ldx #35
vb:     sta WSYNC
        dex
        bne vb
        ; position player 0 on the last VBLANK line
        sta WSYNC
        ldx #7
p0d:    dex
        bne p0d
        sta RESP0
        sta WSYNC
        lda #$60
        sta COLUBK
        lda #$1E
        sta COLUPF
        lda #$44
        sta COLUP0
        lda #$F0
        sta PF1
        lda #%10000001
        sta GRP0
        lda #0
        sta VBLANK
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
        org $FFFC
        .word reset
        .word reset
