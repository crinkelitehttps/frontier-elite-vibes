* Code for the original-game fixes (docs/original-game-fixes.md), as a block
* that tools/atari_patch/patch_atari.py appends to the end of the Atari ST
* binary. Same routines as at the end of fe2/fe2.s, except that the Lprox_log
* diagnostic hcall is left out (it only exists in the host).
*
* Assembled on its own, so it starts at 0 and must be position independent:
* the only absolute reference is the original routine below, patched in as
* a relocated address by patch_atari.py (the equates are text offsets, the
* address in fe2.s.c minus the 28 byte header).
L44af8		equ	$391d0
l7edb4		equ	$73458
l7ee06		equ	$734aa
l7ee7e		equ	$73522

Lgrav_norm:
		addq.w	#1,d7
		lsr.w	#1,d0
		neg.w	d3
		add.w	d7,d3
		neg.w	d3
		subq.w	#1,d3
		rts
Lgrav_mass:
		movem.w	122(a1),d5-6
		add.w	d3,d6
		rts

Lcool_dec:
		lsr.l	#1,d7
		cmp.l	#497,d7
		bls.s	lcool_ok
		move.l	#497,d7
	lcool_ok:	sub.w	d7,278(a0)
		rts

Lprox_log:
		add.l	d1,d0
		add.l	d2,d0
		cmp.l	#$40000000,d0
		bcs.s	lprox_ok
		cmp.l	#$3fffffff,d5
		bcc.s	lprox_ok
		move.l	#$3fffffff,d0
	lprox_ok:	cmp.l	d5,d0
		rts

Lframe_cand:
		btst	#$4,0(a4,d0.w)
		bne.s	lframe_cand_ret
		tst.b	0(a4,d0.w)
		beq.s	lframe_cand_no
		movem.l	d0/a1,-(a7)
L44af8_call:	jsr	L44af8
		cmpi.b	#$1,94(a1)
		bne.s	lframe_cand_no2
		btst	#$4,280(a1)
		movem.l	(a7)+,d0/a1
	lframe_cand_ret:	rts
	lframe_cand_no2:	movem.l	(a7)+,d0/a1
	lframe_cand_no:	cmp.b	d0,d0
		rts

Lpax_late:
		tst.l	12(a0)
		bne.s	lpax_late_dest
		cmpi.b	#$1c,3(a0)
		beq.s	lpax_late_dest
		jmp	l7ee06
	lpax_late_dest:
		cmp.l	4(a0),d2
		beq.s	lpax_late_here
		dbra	d7,lpax_late_next
	lpax_late_here:
		jmp	l7edb4
	lpax_late_next:
		jmp	l7ee7e
