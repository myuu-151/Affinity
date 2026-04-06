@ tex_scanline.s — ARM assembly textured scanline inner loop for GBA Mode 4
@ Runs in IWRAM (0 wait-state, ARM mode) for maximum throughput.
@
@ void tex_scanline_asm(u16* rp, int pairCount, int su, int sv,
@                       int du4, int dv4, const u8* tex,
@                       int texMask, int texShift, int palBase,
@                       int rowOff);
@
@ Writes pairCount pixel-pairs to rp AND rp[rowOff] (row duplication).
@ Samples ONE texel per pair and duplicates to both pixels.
@ du4/dv4 step per 2 pixels (one pair).
@ rowOff in bytes (typically 240 = 120 halfwords * 2 bytes).

    .section .iwram, "ax", %progbits
    .arm
    .align  2
    .global tex_scanline_asm
    .type   tex_scanline_asm, %function

tex_scanline_asm:
    cmp     r1, #0
    bxle    lr

    stmfd   sp!, {r4-r11, lr}     @ save 9 regs (36 bytes)

    @ Load stack args (offset +36 due to 9 pushed regs)
    ldr     r4, [sp, #36]          @ r4 = du4
    ldr     r5, [sp, #40]          @ r5 = dv4
    ldr     r6, [sp, #44]          @ r6 = tex
    ldr     r7, [sp, #48]          @ r7 = texMask
    ldr     r8, [sp, #52]          @ r8 = texShift
    ldr     r9, [sp, #56]          @ r9 = palBase
    ldr     r11, [sp, #60]         @ r11 = rowOff (bytes)

    @ Precompute: lr = texMask << texShift (vMask), r8 = 16 - texShift
    mov     lr, r7, lsl r8         @ lr = vMask
    rsb     r8, r8, #16            @ r8 = vShift

    @ r10 = second row pointer
    add     r10, r0, r11           @ r10 = rp + rowOff

    @ Register map:
    @ r0=rp  r1=count  r2=su  r3=sv  r4=du4  r5=dv4
    @ r6=tex  r7=texMask  r8=vShift  r9=palBase
    @ r10=rp2 (dup row)  r11=scratch  lr=vMask

.Lpair_loop:
    mov     r11, r3, asr r8        @ v = sv >> vShift
    and     r11, r11, lr           @ v &= vMask
    mov     r12, r2, asr #16       @ u = su >> 16
    and     r12, r12, r7           @ u &= texMask
    orr     r11, r11, r12          @ ti = v | u
    ldrb    r11, [r6, r11]         @ px = tex[ti]
    add     r2, r2, r4             @ su += du4
    add     r3, r3, r5             @ sv += dv4
    add     r11, r9, r11           @ px = palBase + texel
    orr     r11, r11, r11, lsl #8  @ px | (px << 8)
    strh    r11, [r0], #2          @ *rp++ = packed
    strh    r11, [r10], #2         @ *rp2++ = packed (row dup)
    subs    r1, r1, #1
    bne     .Lpair_loop

    @ ~18 cycles / 2 pixels = ~9 cycles/pixel (with dual write)

    ldmfd   sp!, {r4-r11, lr}
    bx      lr

.size tex_scanline_asm, .-tex_scanline_asm
