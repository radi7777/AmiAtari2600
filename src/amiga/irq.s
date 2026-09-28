; irq.s - vertical blank interrupt server (see hw.c)
;
; Written in assembler because an exec interrupt server must return with
; the Z flag set to let the servers behind it run (graphics, timers ...),
; which C code cannot guarantee.
;
; is_Data points to struct { ULONG count; struct Task *task; ULONG sigmask; }
; The counter is incremented; if sigmask is not 0 the task is signalled.

        xdef    _hw_vbl_server
        section "CODE",code

_LVOSignal equ -324

_hw_vbl_server:
        addq.l  #1,(a1)
        move.l  8(a1),d0
        beq.s   .done
        move.l  4(a1),a1
        move.l  a6,-(sp)
        move.l  4.w,a6          ; ExecBase (a6 is not guaranteed here)
        jsr     _LVOSignal(a6)
        move.l  (sp)+,a6
.done:
        moveq   #0,d0           ; Z = 1: continue the server chain
        rts

; ---------------------------------------------------------------------
; PC sampling profiler (PROFPC option, only with the OS interrupts off).
; Level 6 autovector handler driven by a CIA-B timer: counts the
; interrupted program counter in 16 byte buckets of our code hunk.

        xdef    _prof_handler
        xdef    _prof_get_vbr
        xref    _prof_base
        xref    _prof_size
        xref    _prof_hist
        xref    _prof_other

        machine 68030

_prof_get_vbr:                  ; called through exec Supervisor()
        movec   vbr,d0
        rte

_prof_handler:
        movem.l d0/a0,-(sp)
        tst.b   $bfdd00         ; CIA-B ICR: read to acknowledge
        move.w  #$2000,$dff09c  ; clear EXTER in INTREQ
        move.w  #$2000,$dff09c
        move.l  10(sp),d0       ; format 0 frame: SR at 8, PC at 10
        sub.l   _prof_base,d0
        cmp.l   _prof_size,d0
        bhs.s   .other
        lsr.l   #4,d0
        move.l  _prof_hist,a0
        addq.l  #1,(a0,d0.l*4)
        bra.s   .out
.other:
        addq.l  #1,_prof_other
.out:
        movem.l (sp)+,d0/a0
        rte
