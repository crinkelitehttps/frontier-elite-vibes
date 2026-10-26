* Code for the original-game fixes (docs/original-game-fixes.md), as a block
* that tools/atari_patch/patch_atari.py appends to the end of the Atari ST
* binary. Same routines as at the end of fe2/fe2.s, except that the Lprox_log
* diagnostic hcall is left out (it only exists in the host) and the time guard
* (Ltguard) uses A6 offsets for A6_scancode_for_GetAscii and A6_pushed_ascii_key.
*
* Assembled on its own, so it starts at 0 and must be position independent:
* the only absolute reference is the original routine below, patched in as
* a relocated address by patch_atari.py (the equates are text offsets, the
* address in fe2.s.c minus the 28 byte header).
L44af8		equ	$391d0
l7edb4		equ	$73458
l7ee06		equ	$734aa
l7ee7e		equ	$73522
L43926		equ	$37ffe

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

* Time acceleration step-down (hook at L4388c l438f0, replaces cmp.w #$8,d6 and
* bgt.s L43926 - same size). d6 is the game's own estimate of the base-2 log of
* the time a ship needs to reach its target at its present speed, less the
* time acceleration (docs/time-acceleration-and-ship-spawning.md 2.7); the
* game's shortcut puts a ship next to its target once it is under 2, which at
* 10000x (200 s a frame, one AI look a frame) is a pirate "out of nowhere". Here a
* ship that flies at the player, with time accelerated and d6 at or under the limit
* of the present rate (Ltguard_tab: about a second of flight left, measured), has the
* game's own Shift+F key for the next lower rate pushed as if pressed (10000x to
* 1000x to 100x to 10x to 1x), each step giving the ship ten times the time to be
* seen in. d6 > 8 goes on to L43926 as before; d6 is kept for the rest.
* Not in hyperspace (any of the keys would set its rate again), and not while a
* key is waiting. While something attacks the game refuses the faster keys with a
* message that ends at 1x anyway: Shift+F1 then.
Ltguard:
		cmp.w	#$8,d6
		bgt.s	ltg_far
		tst.b	16336(a6)
		bne.s	ltg_done
		cmpa.l	674(a6),a1
		bne.s	ltg_done
		cmpi.b	#$8,704(a6)
		beq.s	ltg_done
		move.w	10432(a6),d3
		cmp.w	#12,d3
		bhi.s	ltg_done
		add.w	d3,d3
		move.w	Ltguard_tab(pc,d3.w),d5
		move.w	d5,d4
		asr.w	#8,d4
		cmp.w	d4,d6
		bgt.s	ltg_done
		btst	#$1,10476(a6)
		beq.s	ltg_key
		moveq	#-63,d5
	ltg_key:
		move.b	d5,16337(a6)
		move.b	#$ff,16336(a6)
	ltg_done:
		rts
	ltg_far:
		addq.l	#4,a7
		jmp	L43926
* per time acceleration index 0..12 a word: the d6 limit (signed byte, -128:
* never) and the key (Shift+F1..F4) that takes the rate one step down. Indices
* 3, 7, 10 and 12 are 10x, 100x, 1000x and 10000x.
Ltguard_tab:
		dc.w	$8000
		dc.w	$8000
		dc.w	$8000
		dc.w	$06c1
		dc.w	$8000
		dc.w	$8000
		dc.w	$8000
		dc.w	$05c2
		dc.w	$8000
		dc.w	$8000
		dc.w	$03c3
		dc.w	$8000
		dc.w	$03c4
