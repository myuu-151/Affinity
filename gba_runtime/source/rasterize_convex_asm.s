@ rasterize_convex_asm.s — IWRAM ARM convex polygon rasterizer
@ Calls afn_hline_fast for scanline fill instead of inlining.
    .section .iwram, "ax", %progbits
    .arm
    .align 2
    .global rasterize_convex_asm
    .type   rasterize_convex_asm, %function

.equ FR_L,        0
.equ FR_R,        4
.equ FR_Lh,       8
.equ FR_Rh,       12
.equ FR_Lsteps,   16
.equ FR_Rsteps,   20
.equ FR_PX,       24
.equ FR_PY,       28
.equ FR_COUNT,    32
.equ FR_SIZE,     36
.equ FR_PALIDX,   (FR_SIZE + 36)

rasterize_convex_asm:
    cmp     r3, #3
    bxlt    lr

    stmfd   sp!, {r4-r11, lr}
    sub     sp, sp, #FR_SIZE

    mov     r4, r0
    str     r1, [sp, #FR_PX]
    str     r2, [sp, #FR_PY]
    str     r3, [sp, #FR_COUNT]

    ldr     r11, [sp, #FR_PALIDX]
    and     r11, r11, #0xFF

    @ Find topmost vertex
    mov     r5, #0
    mov     r7, #0
    ldr     r6, [r2]
    mov     r8, r6
    mov     r0, #1

.Lfind_loop:
    cmp     r0, r3
    bge     .Lfind_end
    ldr     r1, [r2, r0, lsl #2]
    cmp     r1, r6
    movlt   r5, r0
    movlt   r6, r1
    cmp     r1, r8
    movgt   r7, r0
    movgt   r8, r1
    add     r0, r0, #1
    b       .Lfind_loop

.Lfind_end:
    cmp     r8, r6
    ble     .Lexit

    str     r5, [sp, #FR_L]
    str     r5, [sp, #FR_R]
    mov     r0, #0
    str     r0, [sp, #FR_Lh]
    str     r0, [sp, #FR_Rh]
    str     r0, [sp, #FR_Lsteps]
    str     r0, [sp, #FR_Rsteps]
    mov     r9, r6

    @ ============================================
    @ LEFT EDGE ADVANCE
    @ ============================================
.Ladv_left:
    ldr     r0, [sp, #FR_Lh]
    cmp     r0, #0
    bne     .Ladv_right

.Lleft_edge:
    ldr     r1, [sp, #FR_L]
    ldr     r3, [sp, #FR_COUNT]
    sub     r2, r1, #1
    cmp     r2, #0
    addlt   r2, r3, #-1

    ldr     r12, [sp, #FR_PY]
    ldr     r0, [r12, r2, lsl #2]
    ldr     lr, [r12, r1, lsl #2]
    cmp     r0, lr
    blt     .Lexit

    ldr     r3, [sp, #FR_Lsteps]
    add     r3, r3, #1
    str     r3, [sp, #FR_Lsteps]
    ldr     r12, [sp, #FR_COUNT]
    cmp     r3, r12
    bgt     .Lexit

    sub     r0, r0, lr
    str     r0, [sp, #FR_Lh]

    ldr     r12, [sp, #FR_PX]
    ldr     r5, [r12, r1, lsl #2]
    mov     r5, r5, lsl #16

    cmp     r0, #1
    movle   r7, #0
    ble     .Lleft_done

    @ Ldx = divLUT[Lh] * (px[N] - px[L])
    ldr     r3, =g_divLUT_ptr
    ldr     r3, [r3]
    cmp     r3, #0
    moveq   r7, #0
    beq     .Lleft_done
    ldr     r3, [r3, r0, lsl #2]
    ldr     r12, [sp, #FR_PX]
    ldr     lr, [r12, r2, lsl #2]
    ldr     r12, [r12, r1, lsl #2]
    sub     lr, lr, r12
    mul     r7, r3, lr

.Lleft_done:
    str     r2, [sp, #FR_L]
    ldr     r0, [sp, #FR_Lh]
    cmp     r0, #0
    beq     .Lleft_edge

    @ ============================================
    @ RIGHT EDGE ADVANCE
    @ ============================================
.Ladv_right:
    ldr     r0, [sp, #FR_Rh]
    cmp     r0, #0
    bne     .Lseg_setup

.Lright_edge:
    ldr     r1, [sp, #FR_R]
    ldr     r3, [sp, #FR_COUNT]
    add     r2, r1, #1
    cmp     r2, r3
    movge   r2, #0

    ldr     r12, [sp, #FR_PY]
    ldr     r0, [r12, r2, lsl #2]
    ldr     lr, [r12, r1, lsl #2]
    cmp     r0, lr
    blt     .Lexit

    ldr     r3, [sp, #FR_Rsteps]
    add     r3, r3, #1
    str     r3, [sp, #FR_Rsteps]
    ldr     r12, [sp, #FR_COUNT]
    cmp     r3, r12
    bgt     .Lexit

    sub     r0, r0, lr
    str     r0, [sp, #FR_Rh]

    ldr     r12, [sp, #FR_PX]
    ldr     r6, [r12, r1, lsl #2]
    mov     r6, r6, lsl #16

    cmp     r0, #1
    movle   r8, #0
    ble     .Lright_done

    ldr     r3, =g_divLUT_ptr
    ldr     r3, [r3]
    cmp     r3, #0
    moveq   r8, #0
    beq     .Lright_done
    ldr     r3, [r3, r0, lsl #2]
    ldr     r12, [sp, #FR_PX]
    ldr     lr, [r12, r2, lsl #2]
    ldr     r12, [r12, r1, lsl #2]
    sub     lr, lr, r12
    mul     r8, r3, lr

.Lright_done:
    str     r2, [sp, #FR_R]
    ldr     r0, [sp, #FR_Rh]
    cmp     r0, #0
    beq     .Lright_edge

    @ ============================================
    @ SEGMENT SETUP
    @ ============================================
.Lseg_setup:
    ldr     r0, [sp, #FR_Lh]
    ldr     r1, [sp, #FR_Rh]
    cmp     r0, r1
    movle   r10, r0
    movgt   r10, r1
    sub     r0, r0, r10
    sub     r1, r1, r10
    str     r0, [sp, #FR_Lh]
    str     r1, [sp, #FR_Rh]

    @ ============================================
    @ SCANLINE LOOP — calls afn_hline_fast
    @ ============================================
.Lscan_loop:
    subs    r10, r10, #1
    blt     .Lscan_done

    cmp     r9, #160
    bge     .Lscan_advance
    cmp     r9, #0
    blt     .Lscan_advance

    mov     r0, r5, asr #16        @ left
    mov     r1, r6, asr #16        @ right

    @ Swap if left > right
    cmp     r0, r1
    eorgt   r0, r0, r1
    eorgt   r1, r1, r0
    eorgt   r0, r0, r1

    @ Clamp and reject
    cmp     r1, #0
    blt     .Lscan_advance
    cmp     r0, #239
    bgt     .Lscan_advance
    cmp     r0, #0
    movlt   r0, #0
    cmp     r1, #239
    movgt   r1, #239

    @ Compute row address: buf + y * 240
    rsb     r2, r9, r9, lsl #4     @ y * 15
    add     r12, r4, r2, lsl #4    @ buf + y * 240

    @ Call afn_hline_fast(row, left, right, palIdx)
    @ r0=left, r1=right already set. Need r0=row, r1=left, r2=right, r3=palIdx
    mov     r3, r11                 @ palIdx
    mov     r2, r1                  @ right
    mov     r1, r0                  @ left
    mov     r0, r12                 @ row
    bl      afn_hline_fast

.Lscan_advance:
    add     r5, r5, r7
    add     r6, r6, r8
    add     r9, r9, #1
    b       .Lscan_loop

.Lscan_done:
    b       .Ladv_left

.Lexit:
    add     sp, sp, #FR_SIZE
    ldmfd   sp!, {r4-r11, lr}
    bx      lr

    .ltorg

.size rasterize_convex_asm, .-rasterize_convex_asm
