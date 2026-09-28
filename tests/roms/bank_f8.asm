; bank_f8.asm - 8K F8 cartridge. Both banks start with the same stub that
; selects bank 1 ($1FF9) and jumps to "main"; only bank 1's main shows the
; expected blue background ($84), bank 0's shows red.
        include "vcs.inc"

        org $E000               ; file offset 0: bank 0
        rorg $F000
main0:  lda #$44
        jmp show
        org $EF00
        rorg $FF00
stub0:  sei
        cld
        ldx #$FF
        txs
        lda $1FF9
        jmp main0
        org $EFFC
        rorg $FFFC
        .word stub0
        .word stub0

        org $F000               ; file offset 4096: bank 1
        rorg $F000
main1:  lda #$84
show:   sta COLUBK
frame:  lda #2
        sta VBLANK
        sta VSYNC
        sta WSYNC
        sta WSYNC
        sta WSYNC
        lda #0
        sta VSYNC
        ldx #37
vb:     sta WSYNC
        dex
        bne vb
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
        org $FF00
        rorg $FF00
stub1:  sei
        cld
        ldx #$FF
        txs
        lda $1FF9
        jmp main1
        org $FFFC
        rorg $FFFC
        .word stub1
        .word stub1
