@ tex_scanline.s — ARM assembly textured scanline inner loop for GBA Mode 4
@ Runs in IWRAM (0 wait-state, ARM mode) for maximum throughput.
@
@ void tex_scanline_asm(u16* rp, int pairCount, int su, int sv,
@                       int du2, int dv2, const u8* tex,
@                       int texMask, int texShift, int palBase);
@
@ Writes pairCount pixel-pairs (2 pixels per halfword) to rp.
@ Samples ONE texel per pair and duplicates to both pixels.
@ Caller passes du2/dv2 pre-doubled (step per 2 pixels).

    .section .iwram, "ax", %progbits
    .arm
    .align  2
    .global tex_scanline_asm
    .type   tex_scanline_asm, %function

tex_scanline_asm:
    cmp     r1, #0
    bxle    lr

    stmfd   sp!, {r4-r10, lr}      @ save 8 regs (32 bytes)

    @ Load stack args (offset +32)
    ldr     r4, [sp, #32]          @ r4 = du2 (pre-doubled)
    ldr     r5, [sp, #36]          @ r5 = dv2 (pre-doubled)
    ldr     r6, [sp, #40]          @ r6 = tex
    ldr     r7, [sp, #44]          @ r7 = texMask
    ldr     r8, [sp, #48]          @ r8 = texShift
    ldr     r9, [sp, #52]          @ r9 = palBase

    @ Precompute combined V shift
    mov     lr, r7, lsl r8         @ lr = texMask << texShift (vMask)
    rsb     r8, r8, #16            @ r8 = 16 - texShift

    @ r0=rp r1=count r2=su r3=sv r4=du2 r5=dv2
    @ r6=tex r7=texMask r8=vShift r9=palBase lr=vMask
    @ r10,r12 = temps

.Lpair_loop:
    mov     r12, r3, asr r8        @ v = sv >> (16 - texShift)        [2cy]
    and     r12, r12, lr           @ v &= vMask                       [1cy]
    mov     r10, r2, asr #16       @ u = su >> 16                     [1cy]
    and     r10, r10, r7           @ u &= texMask                     [1cy]
    orr     r12, r12, r10          @ ti = v | u                       [1cy]
    ldrb    r12, [r6, r12]         @ px = tex[ti]                     [3cy]
    add     r2, r2, r4             @ su += du2 (2-pixel step)         [1cy]
    add     r3, r3, r5             @ sv += dv2 (2-pixel step)         [1cy]
    add     r12, r9, r12           @ px = palBase + texel             [1cy]
    orr     r12, r12, r12, lsl #8  @ duplicate: px | (px << 8)       [1cy]
    strh    r12, [r0], #2          @ *rp++ = packed                   [2cy]
    subs    r1, r1, #1             @                                  [1cy]
    bne     .Lpair_loop            @                                  [3cy]

    @ ~19 cycles / 2 pixels = ~9.5 cycles/pixel

    ldmfd   sp!, {r4-r10, lr}
    bx      lr

.size tex_scanline_asm, .-tex_scanline_asm
