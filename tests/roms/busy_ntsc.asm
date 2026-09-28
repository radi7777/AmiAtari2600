; busy_ntsc.asm - CPU heavy kernel for profiling: about 23 instructions
; and 2 TIA writes per visible line (colour changes computed per line).
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
        lda #$0F
        sta PF1
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
        inc $85
        lda $85
        sta $80
        lda #192
        sta $86
kern:   sta WSYNC
        lda $80         ; 3
        clc             ; 2
        adc #3          ; 2
        sta $80         ; 3
        eor $81         ; 3
        sta COLUPF      ; 3
        lda $81         ; 3
        asl             ; 2
        adc $82         ; 3
        sta $81         ; 3
        ldy $82         ; 3
        iny             ; 2
        sty $82         ; 3
        tya             ; 2
        and #7          ; 2
        tax             ; 2
        lda tab,x       ; 4
        ora $84         ; 3
        sta $84         ; 3
        sta COLUBK      ; 3
        dec $86         ; 5
        bne kern        ; 3
        lda #2
        sta VBLANK
        ldx #30
os:     sta WSYNC
        dex
        bne os
        jmp frame
tab:    .byte $00,$12,$24,$36,$48,$5A,$6C,$7E
        org $FFFC
        .word reset
        .word reset
