#!/usr/bin/env python3
"""
gen_hmove_rom.py - generate an HMOVE timing test ROM (NTSC).

    gen_hmove_rom.py <hm value 0-15> <first cycle> <out.asm>

Groups of 3 lines, one per HMOVE write cycle w = first+3 .. first+40:
  line A: reset P0 (cycle 25), M1 (cycle 40), BL (cycle 55) and write the
          motion value to HMP0/HMM1/HMBL
  line B: WSYNC, d cycles of padding, HMOVE
  line C: nothing (objects are observed here)
Group 0 is a control group without HMOVE. P0 = colour $44, M1 = $C8,
BL = $1E, background = $02 (so the HMOVE blank is visible).
"""
import sys

hm = int(sys.argv[1]) & 15
first = int(sys.argv[2])
out = open(sys.argv[3], "w")
L = []
e = L.append


def pad(n):
    """exactly n cycles (n != 1)"""
    assert n != 1 and n >= 0
    while n > 0:
        if n in (2, 4, 6):
            e("        nop"); n -= 2
        elif n == 3 or n == 5 or n == 7:
            e("        sta $2D"); n -= 3
        else:
            e("        sta $2D"); n -= 3


e('        include "vcs.inc"')
e("RESM1 = $13")
e("RESBL = $14")
e("ENAM1 = $1E")
e("ENABL = $1F")
e("HMM1 = $23")
e("HMBL = $24")
e("        org $F000")
e("reset:  sei")
e("        cld")
e("        ldx #$FF")
e("        txs")
e("        lda #0")
e("clr:    sta 0,x")
e("        dex")
e("        bne clr")
e("        lda #$44")
e("        sta COLUP0")
e("        lda #$C8")
e("        sta COLUP1")
e("        lda #$1E")
e("        sta COLUPF")
e("        lda #$02")
e("        sta COLUBK")
e("        lda #$FF")
e("        sta GRP0")
e("        lda #2")
e("        sta ENAM1")
e("        sta ENABL")
e("frame:  lda #2")
e("        sta VBLANK")
e("        sta VSYNC")
e("        sta WSYNC")
e("        sta WSYNC")
e("        sta WSYNC")
e("        lda #0")
e("        sta VSYNC")
e("        ldx #37")
e("vb:     sta WSYNC")
e("        dex")
e("        bne vb")
e("        lda #0")
e("        sta VBLANK")
e("        ldx #%d" % (hm << 4))
lines = 0
for g in range(39):
    # line A (WSYNC ends at cycle 0 of the line)
    e("        sta WSYNC")
    c = 0
    e("        stx HMP0"); c += 3
    e("        stx HMM1"); c += 3
    e("        stx HMBL"); c += 3
    pad(22 - c); c = 22
    e("        sta RESP0"); c += 3          # write on cycle 25
    pad(37 - c); c = 37
    e("        sta RESM1"); c += 3          # 40
    pad(52 - c); c = 52
    e("        sta RESBL"); c += 3          # 55
    # line B
    e("        sta WSYNC")
    if g > 0:
        w = first + g - 1 + 3              # cycle of the HMOVE write
        if w == 4:
            e("        .byte $8D,$2A,$00")    # sta HMOVE (absolute, 4 cycles)
        else:
            pad(w - 3)
            e("        sta HMOVE")
    # line C
    e("        sta WSYNC")
    lines += 3
e("        ldx #%d" % (192 - lines))
e("kern:   sta WSYNC")
e("        dex")
e("        bne kern")
e("        lda #2")
e("        sta VBLANK")
e("        ldx #30")
e("os:     sta WSYNC")
e("        dex")
e("        bne os")
e("        jmp frame")
e("        org $FFFC")
e("        .word reset")
e("        .word reset")
out.write("\n".join(L) + "\n")
