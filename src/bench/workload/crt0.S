/* Benchmark entry: copy .data, clear .bss, run bench_main, then park the CPU
 * with the checksum in D0 (and at RAM 0x400) via STOP, which the harness sees
 * as cpu->regs.stopped. Interrupts stay masked throughout. */
    .section .text.start,"ax"
    .globl _start
_start:
    move.w  #0x2700,%sr
    lea     _data_load,%a0
    lea     _data_start,%a1
    lea     _data_end,%a2
1:  cmp.l   %a2,%a1
    bcc.s   2f
    move.w  (%a0)+,(%a1)+
    bra.s   1b
2:  lea     _bss_start,%a1
    lea     _bss_end,%a2
3:  cmp.l   %a2,%a1
    bcc.s   4f
    clr.w   (%a1)+
    bra.s   3b
4:  jsr     bench_main
    move.l  %d0,0x400
5:  stop    #0x2700
    bra.s   5b
