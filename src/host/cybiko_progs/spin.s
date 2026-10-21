; Spin forever (for tests that drive the hardware from C).
	.h8300s
	.section .text
	.org 0
	.long start
	.org 0x200                ; past the vector table (92 vectors)
start:
	mov.l #0xfffc00, er7
loop:
	bra loop
