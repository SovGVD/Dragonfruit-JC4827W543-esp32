; Send "OK\n" on SCI2 at 57600 8N1, polling TDRE.
	.h8300s
	.section .text
	.org 0
	.long start
	.org 0x200                ; past the vector table (92 vectors)
start:
	mov.l #0xfffc00, er7
	mov.b #0x00, r0l         ; SMR2: async 8N1, phi/1
	mov.b r0l, @0xffff88:8
	mov.b #9, r0l            ; BRR2: 57600 baud
	mov.b r0l, @0xffff89:8
	mov.b #0x20, r0l         ; SCR2: TE
	mov.b r0l, @0xffff8a:8
	mov.l #msg, er1
next:
	mov.b @er1+, r0l
	beq done
wait:
	btst #7, @0xffff8c:8     ; TDRE
	beq wait
	mov.b r0l, @0xffff8b:8
	bclr #7, @0xffff8c:8
	bra next
done:
	sleep
	bra done
msg:	.asciz "OK\n"
