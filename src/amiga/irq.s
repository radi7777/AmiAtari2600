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
