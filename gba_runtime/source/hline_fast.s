@ hline_fast.s — ARM assembly flat-fill scanline for GBA Mode 4 (VRAM)
@ Runs in IWRAM (0 wait-state, ARM mode) for maximum throughput.
@ Uses word-aligned STR/STM for bulk fill — ~0.5 cy/pixel vs ~1.5 with strb.
@ Edge pixels use read-modify-write to preserve neighbors.
@
@ void afn_hline_fast(u16* row, int left, int right, u8 palIdx);

    .section .iwram, "ax", %progbits
    .arm
    .align  2
    .global afn_hline_fast
    .type   afn_hline_fast, %function

afn_hline_fast:
    @ r0 = row, r1 = left, r2 = right, r3 = palIdx
    subs    r12, r2, r1
    bxlt    lr

    add     r0, r0, r1              @ r0 = byte ptr to first pixel
    add     r12, r12, #1            @ r12 = pixel count

    @ Broadcast palIdx to all 4 byte lanes
    and     r3, r3, #0xFF
    orr     r3, r3, r3, lsl #8     @ 0xCCCC
    orr     r3, r3, r3, lsl #16    @ 0xCCCCCCCC

    @ --- Step 1: Align to halfword (handle odd left byte) ---
    tst     r0, #1
    beq     .Lhw_ok
    @ Pixel is at high byte of halfword at (ptr-1)
    ldrh    r2, [r0, #-1]
    and     r2, r2, #0x00FF         @ keep low byte (left neighbor)
    orr     r2, r2, r3, lsl #8     @ set high byte = palIdx
    strh    r2, [r0, #-1]
    add     r0, r0, #1
    subs    r12, r12, #1
    bxle    lr
.Lhw_ok:

    @ --- Step 2: Peel odd trailing pixel ---
    tst     r12, #1
    beq     .Lright_ok
    sub     r12, r12, #1
    add     r2, r0, r12             @ r2 = ptr to last pixel (at low byte of halfword)
    ldrh    r1, [r2]
    bic     r1, r1, #0xFF           @ clear low byte, keep high byte
    orr     r1, r1, r3, lsr #24    @ set low byte = palIdx
    strh    r1, [r2]
    cmp     r12, #0
    bxeq    lr
.Lright_ok:

    @ --- Step 3: Align to word (handle extra halfword) ---
    @ r0 is halfword-aligned, r12 is even >= 2
    tst     r0, #2
    beq     .Lword_ok
    strh    r3, [r0], #2            @ 2 pixels
    subs    r12, r12, #2
    bxle    lr
.Lword_ok:

    @ --- Step 4: Bulk word fill ---
    @ r0 is word-aligned, r12 is even pixel count
    @ Save remaining-halfword flag, convert to word count
    movs    r2, r12, lsr #2         @ r2 = word count (4 px each)
    beq     .Lfinal_hw

    @ For >= 4 words (16 px), use STM
    cmp     r2, #4
    blt     .Lstr_loop

    stmfd   sp!, {r4-r6}
    mov     r4, r3
    mov     r5, r3
    mov     r6, r3

    subs    r2, r2, #4
.Lstm_loop:
    stmia   r0!, {r3-r6}           @ 16 pixels
    subs    r2, r2, #4
    bge     .Lstm_loop

    adds    r2, r2, #4
    ldmfd   sp!, {r4-r6}
    cmp     r2, #0
    beq     .Lfinal_hw

.Lstr_loop:
    str     r3, [r0], #4           @ 4 pixels
    subs    r2, r2, #1
    bne     .Lstr_loop

.Lfinal_hw:
    @ Check for remaining 2 pixels
    tst     r12, #2
    strneh  r3, [r0]
    bx      lr

.size afn_hline_fast, .-afn_hline_fast
