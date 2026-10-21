; TPU channel 3 compare-match interrupt every 1000 states, interrupt
; control mode 2. ER5 counts interrupts.
	.h8300s
	.section .text
	.org 0
	.long start
	.org 48*4                ; TGI3A
	.long tgi3a
	.org 0x200                ; past the vector table (92 vectors)
start:
	mov.l #0xfffc00, er7
	mov.b #0x21, r0l         ; SYSCR: INTM=2, RAME
	mov.b r0l, @0xffff39:8
	mov.b #0x05, r0l         ; IPRG low nibble (TPU3) = 5
	mov.b r0l, @0xfeca:16
	mov.b #0x20, r0l         ; TCR3: clear on TGRA, phi/1
	mov.b r0l, @0xfe80:16
	mov.w #999, r0           ; TGRA3
	mov.w r0, @0xfe88:16
	mov.b #0x01, r0l         ; TIER3: TGIEA
	mov.b r0l, @0xfe84:16
	sub.l er5, er5
	mov.b #0x08, r0l         ; TSTR: start channel 3
	mov.b r0l, @0xffffc0:8
	ldc #0, exr
loop:
	sleep
	bra loop
tgi3a:
	bclr #0, @0xfe85:16      ; clear TGFA
	inc.l #1, er5
	rte
