; Receive bytes on SCI2 by RXI interrupt (mode 0) into RAM at 0x400000.
	.h8300s
	.section .text
	.org 0
	.long start
	.org 89*4                ; RXI2
	.long rxi2
	.org 0x200                ; past the vector table (92 vectors)
start:
	mov.l #0xfffc00, er7
	mov.l #0x400000, er6
	mov.b #0x00, r0l
	mov.b r0l, @0xffff88:8
	mov.b #9, r0l
	mov.b r0l, @0xffff89:8
	mov.b #0x50, r0l         ; SCR2: RIE, RE
	mov.b r0l, @0xffff8a:8
	andc #0x7f, ccr
loop:
	sleep
	bra loop
rxi2:
	mov.b @0xffff8d:8, r0l   ; RDR2
	mov.b r0l, @er6
	inc.l #1, er6
	bclr #6, @0xffff8c:8     ; RDRF
	rte
