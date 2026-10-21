; Software square wave on P15, toggled from the TMR0 compare-match
; interrupt every 1152 states (phi/8, TCORA=143): 8000 Hz.
	.h8300s
	.section .text
	.org 0
	.long start
	.org 64*4                ; CMIA0
	.long cmia0
	.org 0x200                ; past the vector table (92 vectors)
start:
	mov.l #0xfffc00, er7
	mov.b #0x20, r0l
	mov.b r0l, @0xfeb0:16    ; P1DDR: P15 output
	sub.b r1l, r1l
	mov.b #143, r0l
	mov.b r0l, @0xffffb4:8   ; TCORA0
	mov.b #0x49, r0l         ; TCR0: CMIEA, clear on A, phi/8
	mov.b r0l, @0xffffb0:8
	andc #0x7f, ccr
loop:
	sleep
	bra loop
cmia0:
	bclr #6, @0xffffb2:8     ; CMFA
	xor.b #0x20, r1l
	mov.b r1l, @0xffff60:8   ; P1DR
	rte
