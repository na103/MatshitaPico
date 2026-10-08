| =============================================================================
| MatshitaPico -- Solid-state CD-ROM drive replacement for the Commodore CDTV
| Copyright (c) 2026 Nicola Avanzi (na103)
| Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
| See LICENSE.
| =============================================================================
| crt0.S -- MINIMAL Kickstart 1.3 entry for mpmount (NO utility.library).
|
| Bebbo's standard crt0 auto-opens utility.library, ABSENT on KS 1.3 (it is a
| 2.0 library) -> crash BEFORE main on the CDTV. This entry opens ONLY
| dos.library, calls _run() and closes it. It MUST be linked FIRST: on the Amiga
| the entry point is offset 0 of the first CODE hunk (DOS JSRs to it, returns via
| the stack, rc in d0).
|
| _SysBase/_DOSBase are defined in mpmount.c (used by the proto inlines).
| =============================================================================
	.globl	__start
	.globl	_run
	.globl	_SysBase
	.globl	_DOSBase
	.text
__start:
	movea.l	4,%a6			| ExecBase (AbsExecBase @ 4)
	move.l	%a6,_SysBase
	lea	dosname,%a1
	moveq	#0,%d0
	jsr	-552(%a6)		| OpenLibrary("dos.library", 0)
	move.l	%d0,_DOSBase
	beq.s	no_dos
	jsr	_run			| run; rc in d0
	move.l	%d0,%d2			| save rc
	movea.l	_DOSBase,%a1
	movea.l	4,%a6
	jsr	-414(%a6)		| CloseLibrary(DOSBase)
	move.l	%d2,%d0			| restore rc
	rts
no_dos:
	moveq	#20,%d0
	rts
dosname:
	.asciz	"dos.library"
	.align	2

| -----------------------------------------------------------------------------
| SOFTWARE __udivsi3 / __umodsi3 (32/32 unsigned, shift-subtract).
| Bebbo implements these with utility.library/UDivMod32, ABSENT on KS 1.3
| (UtilityBase=NULL -> jump into nowhere = crash). Defined here (object linked
| first), the linker uses OURS and does not pull in the libgcc ones.
| libgcc m68k convention: arguments on the stack, result in d0.
| -----------------------------------------------------------------------------
	.globl	___udivsi3
	.globl	___umodsi3
___umodsi3:
	movem.l	%d2-%d3,-(%sp)
	move.l	12(%sp),%d0		| dividend
	move.l	16(%sp),%d1		| divisor
	bsr.s	udivmod
	move.l	%d2,%d0			| remainder -> d0
	movem.l	(%sp)+,%d2-%d3
	rts
___udivsi3:
	movem.l	%d2-%d3,-(%sp)
	move.l	12(%sp),%d0		| dividend
	move.l	16(%sp),%d1		| divisor
	bsr.s	udivmod
	movem.l	(%sp)+,%d2-%d3		| quotient already in d0
	rts
| in:  d0=dividend d1=divisor   out: d0=quotient d2=remainder  (clobbers d3)
udivmod:
	moveq	#0,%d2			| remainder = 0
	tst.l	%d1
	beq.s	udm_done		| divisor 0 -> no crash (never 0 in our use)
	moveq	#31,%d3
udm_loop:
	add.l	%d0,%d0			| shift dividend left, bit31 -> X
	addx.l	%d2,%d2			| shift into remainder
	cmp.l	%d1,%d2
	bcs.s	udm_skip
	sub.l	%d1,%d2
	addq.l	#1,%d0			| set quotient bit
udm_skip:
	dbra	%d3,udm_loop		| 32 iterations
udm_done:
	rts
