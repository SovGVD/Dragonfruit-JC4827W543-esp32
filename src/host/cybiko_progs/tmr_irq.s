; TMR0 compare match A every 800 states (phi/8, TCORA=99), mode 0.
; ER5 counts interrupts.
	.h8300s
	.section .text
	.org 0
	.long start
	.org 64*4                ; CMIA0
	.long cmia0
	.org 0x200                ; past the vector table (92 vectors)
start:
	mov.l #0xfffc00, er7
	sub.l er5, er5
	mov.b #99, r0l
	mov.b r0l, @0xffffb4:8   ; TCORA0
	mov.b #0x49, r0l         ; TCR0: CMIEA, clear on A, phi/8
	mov.b r0l, @0xffffb0:8
	andc #0x7f, ccr
loop:
	sleep
	bra loop
cmia0:
	bclr #6, @0xffffb2:8     ; CMFA
	inc.l #1, er5
	rte
