@ hline_fast.s — ARM assembly flat-fill scanline for GBA Mode 4 (VRAM)
@ Runs in IWRAM (0 wait-state, ARM mode) for maximum throughput.
@ Uses the GBA VRAM strb trick: writing a byte to VRAM duplicates it
@ to both halves of the halfword, filling 2 identical pixels per store.
@
@ void afn_hline_fast(u16* row, int left, int right, u8 palIdx);
@
@ row     = pointer to start of scanline in VRAM (halfword-aligned, u16*)
@ left    = leftmost pixel x (0-239)
@ right   = rightmost pixel x (0-239), inclusive
@ palIdx  = palette index (0-255)
@
@ ~3 cycles/pixel for bulk fill vs ~9.5 in C

    .section .iwram, "ax", %progbits
    .arm
    .align  2
    .global afn_hline_fast
    .type   afn_hline_fast, %function

afn_hline_fast:
    @ r0 = row (u16*, already byte address), r1 = left, r2 = right, r3 = palIdx
    subs    r12, r2, r1             @ r12 = right - left
    bxlt    lr                      @ return if right < left

    @ Compute byte pointer: ptr = (u8*)row + left
    @ r0 is already the byte address of the row (C does u16* arithmetic)
    add     r0, r0, r1             @ r0 = byte ptr to first pixel
    add     r12, r12, #1           @ r12 = pixel count (right - left + 1)

    @ Handle odd left pixel (ptr not halfword-aligned)
    tst     r0, #1                 @ test bit 0 of ptr
    beq     .Lcheck_right
    @ Left pixel is at odd byte — read-modify-write the halfword
    ldrh    r2, [r0, #-1]          @ read halfword at (ptr & ~1)
    and     r2, r2, #0x00FF        @ keep low byte
    orr     r2, r2, r3, lsl #8    @ set high byte = palIdx
    strh    r2, [r0, #-1]          @ write back
    add     r0, r0, #1             @ advance ptr past this pixel
    subs    r12, r12, #1           @ one fewer pixel
    bxle    lr                     @ done if no pixels left

.Lcheck_right:
    @ Handle odd right pixel (odd count remaining means last pixel is solo)
    tst     r12, #1                @ odd pixel count?
    beq     .Lbulk_fill
    @ Last pixel is solo — read-modify-write
    sub     r12, r12, #1           @ exclude last pixel from bulk
    add     r2, r0, r12            @ r2 = ptr to last pixel byte
    ldrh    r1, [r2]               @ read halfword at last pixel
    and     r1, r1, #0xFF00        @ keep high byte
    orr     r1, r1, r3             @ set low byte = palIdx
    strh    r1, [r2]               @ write back
    cmp     r12, #0
    bxeq    lr                     @ done if that was the only remaining pixel

.Lbulk_fill:
    @ Bulk fill: ptr is now halfword-aligned, r12 is even pixel count
    @ Use strb VRAM trick: strb writes byte to both halves of halfword
    movs    r12, r12, lsr #1       @ r12 = pair count (pixels / 2)
    bxeq    lr

.Lfill_loop:
    strb    r3, [r0], #2           @ VRAM trick: fills 2 pixels     [2cy]
    subs    r12, r12, #1           @                                [1cy]
    bne     .Lfill_loop            @                                [3cy taken]
    @ ~6 cycles per 2 pixels = ~3 cycles/pixel

    bx      lr

.size afn_hline_fast, .-afn_hline_fast
