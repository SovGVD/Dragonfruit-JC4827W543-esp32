; TPU channel 1 toggles TIOCB1 (the speaker) on every TGRA clear:
; phi/64, TGRA = 143 -> 18432000 / 64 / 144 / 2 = 1000 Hz.
	.h8300s
	.section .text
	.org 0
	.long start
	.org 0x200                ; past the vector table (92 vectors)
start:
	mov.l #0xfffc00, er7
	mov.b #0x23, r0l         ; TCR1: clear on TGRA, phi/64
	mov.b r0l, @0xffffe0:8
	mov.b #0x30, r0l         ; TIOR1: IOB = toggle on compare match
	mov.b r0l, @0xffffe2:8
	mov.w #143, r0
	mov.w r0, @0xffe8:16     ; TGRA1
	mov.w #10, r0
	mov.w r0, @0xffea:16     ; TGRB1
	mov.b #0x02, r0l         ; TSTR: start channel 1
	mov.b r0l, @0xffffc0:8
loop:
	sleep
	bra loop
