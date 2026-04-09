@ rasterize_convex_tex_asm.s — IWRAM ARM textured convex polygon rasterizer
@ Edge walking in ASM with UV interpolation, calls tex_scanline_asm per span.
@ Uses FIQ register banking: edge deltas stored in FIQ-banked r8-r12, r14
@ to eliminate 6 stack loads per scanline advance (~12 cycles/scanline saved).
@ Eliminates C-level overhead from rasterize_convex_tex.
@
@ void rasterize_convex_tex_asm(u16* buf, int* px, int* py, int* pu, int* pv,
@                                int count, const u8* tex, int texMask,
@                                int texShift, u8 palBase);

    .section .iwram, "ax", %progbits
    .arm
    .align 2
    .global rasterize_convex_tex_asm
    .type   rasterize_convex_tex_asm, %function

@ Stack frame layout
.equ TF_L,        0
.equ TF_R,        4
.equ TF_Lh,       8
.equ TF_Rh,       12
.equ TF_Lsteps,   16
.equ TF_Rsteps,   20
.equ TF_PX,       24
.equ TF_PY,       28
.equ TF_PU,       32
.equ TF_PV,       36
.equ TF_COUNT,    40
.equ TF_BUF,      44
.equ TF_TEX,      48
.equ TF_TEXMASK,  52
.equ TF_TEXSHIFT, 56
.equ TF_PALBASE,  60
.equ TF_LDX,      64
.equ TF_RDX,      68
.equ TF_LDU,      72
.equ TF_RDU,      76
.equ TF_LDV,      80
.equ TF_RDV,      84
@ Edge UV state (16.16 fixed)
.equ TF_LU,       88
.equ TF_LV,       92
.equ TF_RU,       96
.equ TF_RV,       100
.equ TF_SIZE,     104

@ Caller pushed 9 regs (36 bytes), then we alloc TF_SIZE
.equ ORIG_ARGS,   (TF_SIZE + 36)

rasterize_convex_tex_asm:
    @ r0=buf, r1=px, r2=py, r3=pu
    stmfd   sp!, {r4-r11, lr}
    sub     sp, sp, #TF_SIZE

    @ Load count
    ldr     r4, [sp, #ORIG_ARGS + 4]   @ count
    cmp     r4, #3
    blt     .Texit

    @ Store all params
    str     r0, [sp, #TF_BUF]
    str     r1, [sp, #TF_PX]
    str     r2, [sp, #TF_PY]
    str     r3, [sp, #TF_PU]
    ldr     r5, [sp, #ORIG_ARGS + 0]   @ pv
    str     r5, [sp, #TF_PV]
    str     r4, [sp, #TF_COUNT]
    ldr     r5, [sp, #ORIG_ARGS + 8]   @ tex
    str     r5, [sp, #TF_TEX]
    ldr     r5, [sp, #ORIG_ARGS + 12]  @ texMask
    str     r5, [sp, #TF_TEXMASK]
    ldr     r5, [sp, #ORIG_ARGS + 16]  @ texShift
    str     r5, [sp, #TF_TEXSHIFT]
    ldr     r5, [sp, #ORIG_ARGS + 20]  @ palBase
    str     r5, [sp, #TF_PALBASE]

    @ Find topmost vertex
    ldr     r2, [sp, #TF_PY]
    mov     r5, #0
    ldr     r6, [r2]                    @ minY
    mov     r7, #0
    ldr     r8, [r2]                    @ maxY
    mov     r0, #1
.Tfind:
    cmp     r0, r4
    bge     .Tfind_end
    ldr     r1, [r2, r0, lsl #2]
    cmp     r1, r6
    movlt   r5, r0
    movlt   r6, r1
    cmp     r1, r8
    movgt   r7, r0
    movgt   r8, r1
    add     r0, r0, #1
    b       .Tfind
.Tfind_end:
    cmp     r8, r6
    ble     .Texit

    @ Init state
    str     r5, [sp, #TF_L]
    str     r5, [sp, #TF_R]
    mov     r0, #0
    str     r0, [sp, #TF_Lh]
    str     r0, [sp, #TF_Rh]
    str     r0, [sp, #TF_Lsteps]
    str     r0, [sp, #TF_Rsteps]

    @ Register usage during scanline loop:
    @ r4 = Lx (16.16)   r5 = Rx (16.16)
    @ r6 = segment height counter
    @ r9 = y
    @ r10, r11 = scratch
    @ Edge UV and slopes live on stack (TF_LU/LV/RU/RV and TF_LDx)
    mov     r9, r6                      @ y = minY (r6 from find)
    @ r6 will be reused as segment counter below

    @ ============================================
    @ LEFT EDGE ADVANCE
    @ ============================================
.Tadv_left:
    ldr     r0, [sp, #TF_Lh]
    cmp     r0, #0
    bne     .Tadv_right

.Tleft_edge:
    ldr     r1, [sp, #TF_L]
    ldr     r3, [sp, #TF_COUNT]
    sub     r2, r1, #1
    cmp     r2, #0
    addlt   r2, r3, #-1

    ldr     r12, [sp, #TF_PY]
    ldr     r0, [r12, r2, lsl #2]      @ py[N]
    ldr     lr, [r12, r1, lsl #2]      @ py[L]
    cmp     r0, lr
    blt     .Texit

    ldr     r3, [sp, #TF_Lsteps]
    add     r3, r3, #1
    str     r3, [sp, #TF_Lsteps]
    ldr     r12, [sp, #TF_COUNT]
    cmp     r3, r12
    bgt     .Texit

    sub     r0, r0, lr                  @ Lh
    str     r0, [sp, #TF_Lh]

    @ Load Lx from vertex
    ldr     r12, [sp, #TF_PX]
    ldr     r4, [r12, r1, lsl #2]
    mov     r4, r4, lsl #16

    @ Load Lu, Lv from vertex
    ldr     r12, [sp, #TF_PU]
    ldr     r10, [r12, r1, lsl #2]
    mov     r10, r10, lsl #16
    str     r10, [sp, #TF_LU]
    ldr     r12, [sp, #TF_PV]
    ldr     r10, [r12, r1, lsl #2]
    mov     r10, r10, lsl #16
    str     r10, [sp, #TF_LV]

    cmp     r0, #1
    ble     .Tleft_no_slope

    @ Compute slopes using divLUT
    ldr     r3, =g_divLUT_ptr
    ldr     r3, [r3]
    cmp     r3, #0
    beq     .Tleft_no_slope
    ldr     r3, [r3, r0, lsl #2]       @ inv

    @ Ldx
    ldr     r12, [sp, #TF_PX]
    ldr     lr, [r12, r2, lsl #2]
    ldr     r12, [r12, r1, lsl #2]
    sub     lr, lr, r12
    mul     r10, r3, lr
    str     r10, [sp, #TF_LDX]

    @ Ldu
    ldr     r12, [sp, #TF_PU]
    ldr     lr, [r12, r2, lsl #2]
    ldr     r12, [r12, r1, lsl #2]
    sub     lr, lr, r12
    mul     r10, r3, lr
    str     r10, [sp, #TF_LDU]

    @ Ldv
    ldr     r12, [sp, #TF_PV]
    ldr     lr, [r12, r2, lsl #2]
    ldr     r12, [r12, r1, lsl #2]
    sub     lr, lr, r12
    mul     r10, r3, lr
    str     r10, [sp, #TF_LDV]
    b       .Tleft_done

.Tleft_no_slope:
    mov     r10, #0
    str     r10, [sp, #TF_LDX]
    str     r10, [sp, #TF_LDU]
    str     r10, [sp, #TF_LDV]

.Tleft_done:
    str     r2, [sp, #TF_L]
    ldr     r0, [sp, #TF_Lh]
    cmp     r0, #0
    beq     .Tleft_edge

    @ ============================================
    @ RIGHT EDGE ADVANCE
    @ ============================================
.Tadv_right:
    ldr     r0, [sp, #TF_Rh]
    cmp     r0, #0
    bne     .Tseg_setup

.Tright_edge:
    ldr     r1, [sp, #TF_R]
    ldr     r3, [sp, #TF_COUNT]
    add     r2, r1, #1
    cmp     r2, r3
    movge   r2, #0

    ldr     r12, [sp, #TF_PY]
    ldr     r0, [r12, r2, lsl #2]
    ldr     lr, [r12, r1, lsl #2]
    cmp     r0, lr
    blt     .Texit

    ldr     r3, [sp, #TF_Rsteps]
    add     r3, r3, #1
    str     r3, [sp, #TF_Rsteps]
    ldr     r12, [sp, #TF_COUNT]
    cmp     r3, r12
    bgt     .Texit

    sub     r0, r0, lr
    str     r0, [sp, #TF_Rh]

    ldr     r12, [sp, #TF_PX]
    ldr     r5, [r12, r1, lsl #2]
    mov     r5, r5, lsl #16

    ldr     r12, [sp, #TF_PU]
    ldr     r10, [r12, r1, lsl #2]
    mov     r10, r10, lsl #16
    str     r10, [sp, #TF_RU]
    ldr     r12, [sp, #TF_PV]
    ldr     r10, [r12, r1, lsl #2]
    mov     r10, r10, lsl #16
    str     r10, [sp, #TF_RV]

    cmp     r0, #1
    ble     .Tright_no_slope

    ldr     r3, =g_divLUT_ptr
    ldr     r3, [r3]
    cmp     r3, #0
    beq     .Tright_no_slope
    ldr     r3, [r3, r0, lsl #2]

    ldr     r12, [sp, #TF_PX]
    ldr     lr, [r12, r2, lsl #2]
    ldr     r12, [r12, r1, lsl #2]
    sub     lr, lr, r12
    mul     r10, r3, lr
    str     r10, [sp, #TF_RDX]

    ldr     r12, [sp, #TF_PU]
    ldr     lr, [r12, r2, lsl #2]
    ldr     r12, [r12, r1, lsl #2]
    sub     lr, lr, r12
    mul     r10, r3, lr
    str     r10, [sp, #TF_RDU]

    ldr     r12, [sp, #TF_PV]
    ldr     lr, [r12, r2, lsl #2]
    ldr     r12, [r12, r1, lsl #2]
    sub     lr, lr, r12
    mul     r10, r3, lr
    str     r10, [sp, #TF_RDV]
    b       .Tright_done

.Tright_no_slope:
    mov     r10, #0
    str     r10, [sp, #TF_RDX]
    str     r10, [sp, #TF_RDU]
    str     r10, [sp, #TF_RDV]

.Tright_done:
    str     r2, [sp, #TF_R]
    ldr     r0, [sp, #TF_Rh]
    cmp     r0, #0
    beq     .Tright_edge

    .ltorg

    @ ============================================
    @ SEGMENT SETUP
    @ ============================================
.Tseg_setup:
    ldr     r0, [sp, #TF_Lh]
    ldr     r1, [sp, #TF_Rh]
    cmp     r0, r1
    movle   r6, r0
    movgt   r6, r1
    sub     r0, r0, r6
    sub     r1, r1, r6
    str     r0, [sp, #TF_Lh]
    str     r1, [sp, #TF_Rh]

    @ Store edge deltas in FIQ-banked r8-r12, r14 for fast scanline advance.
    @ FIQ regs are separate from System regs — r0-r7 are shared (accessible in both).
    @ Save r6 (segment counter) on stack so we have 6 low regs for the transfer.
    push    {r6}
    ldr     r0, [sp, #4 + TF_LDX]
    ldr     r1, [sp, #4 + TF_RDX]
    ldr     r2, [sp, #4 + TF_LDU]
    ldr     r3, [sp, #4 + TF_RDU]
    ldr     r6, [sp, #4 + TF_LDV]
    ldr     r7, [sp, #4 + TF_RDV]
    msr     cpsr_c, #0x11          @ FIQ mode (IRQ still enabled, FIQ regs banked)
    mov     r8, r0                 @ r8_fiq  = LDX
    mov     r9, r1                 @ r9_fiq  = RDX
    mov     r10, r2                @ r10_fiq = LDU
    mov     r11, r3                @ r11_fiq = RDU
    mov     r12, r6                @ r12_fiq = LDV
    mov     r14, r7                @ r14_fiq = RDV
    msr     cpsr_c, #0x1F          @ System mode
    pop     {r6}

    .ltorg

    @ ============================================
    @ SCANLINE LOOP
    @ ============================================
.Tscan_loop:
    subs    r6, r6, #1
    blt     .Tscan_done

    cmp     r9, #160
    bge     .Tscan_advance
    cmp     r9, #0
    blt     .Tscan_advance

    @ Get left/right X
    mov     r0, r4, asr #16            @ xl
    mov     r1, r5, asr #16            @ xr

    @ Get left/right UVs
    ldr     r2, [sp, #TF_LU]
    mov     r2, r2, asr #16            @ ul
    ldr     r3, [sp, #TF_RU]
    mov     r3, r3, asr #16            @ ur
    ldr     r7, [sp, #TF_LV]
    mov     r7, r7, asr #16            @ vl
    ldr     r8, [sp, #TF_RV]
    mov     r8, r8, asr #16            @ vr

    @ Swap if left > right
    cmp     r0, r1
    bgt     .Tswap
    b       .Tno_swap
.Tswap:
    eor     r0, r0, r1
    eor     r1, r1, r0
    eor     r0, r0, r1
    eor     r2, r2, r3
    eor     r3, r3, r2
    eor     r2, r2, r3
    eor     r7, r7, r8
    eor     r8, r8, r7
    eor     r7, r7, r8
.Tno_swap:

    @ Screen reject
    cmp     r1, #0
    blt     .Tscan_advance
    cmp     r0, #239
    bgt     .Tscan_advance
    cmp     r1, #239
    movgt   r1, #239

    @ Compute span
    sub     r10, r1, r0                 @ fullSpan
    cmp     r10, #0
    ble     .Tscan_advance

    @ Compute du2/dv2 per pixel using divLUT
    ldr     r11, =g_divLUT_ptr
    ldr     r11, [r11]
    cmp     r10, #240
    ldrle   r11, [r11, r10, lsl #2]    @ inv = divLUT[span]
    movgt   r11, #0

    sub     r12, r3, r2                 @ ur - ul
    mul     r12, r11, r12
    mov     r12, r12, asr #8           @ du2

    sub     lr, r8, r7                  @ vr - vl
    mul     lr, r11, lr
    mov     lr, lr, asr #8             @ dv2

    @ su = ul << 8, sv = vl << 8
    mov     r2, r2, lsl #8             @ su
    mov     r3, r7, lsl #8             @ sv

    @ Clamp left
    cmp     r0, #0
    bge     .Tnoclamp
    rsb     r7, r0, #0                 @ skip = -xl
    mla     r2, r12, r7, r2            @ su += du2 * skip
    mla     r3, lr, r7, r3             @ sv += dv2 * skip
    mov     r0, #0
.Tnoclamp:

    @ Handle odd start pixel
    @ r0=xl, r1=xr, r2=su, r3=sv, r12=du2, lr=dv2
    @ Compute row pointer: buf + y*240 bytes
    ldr     r10, [sp, #TF_BUF]
    rsb     r7, r9, r9, lsl #4         @ y*15
    add     r10, r10, r7, lsl #4       @ row = buf + y*240

    @ Align xl up to even, xr down to odd — skip odd boundary pixels
    @ (loses at most 1 pixel per edge, invisible on GBA)
    tst     r0, #1
    addne   r0, r0, #1                  @ xl++ if odd
    addne   r2, r2, r12                 @ su += du2
    addne   r3, r3, lr                  @ sv += dv2
    orr     r1, r1, #1                  @ xr |= 1 (round up to odd = include pair)

    @ Now xl is even, xr is odd (or xl > xr if no pairs left)
    @ Pair count
    sub     r7, r1, r0
    add     r7, r7, #1
    movs    r7, r7, asr #1             @ pairCount = (xr - xl + 1) / 2
    ble     .Tscan_advance

    cmp     r7, #120
    movgt   r7, #120

    @ du4 = du2 * 2, dv4 = dv2 * 2
    mov     r8, r12, lsl #1            @ du4
    mov     r11, lr, lsl #1            @ dv4

    @ Compute rp = row + (xl >> 1) as u16* offset
    @ row (r10) is byte address. rp = (u16*)(row) + (xl >> 1)
    @ In byte terms: rp_addr = row + (xl & ~1)  [since each u16 = 2 bytes = 2 pixels]
    bic     r0, r0, #1                 @ xl & ~1 (should already be even)
    add     r0, r10, r0                @ rp byte address

    @ Call tex_scanline_asm(rp, pairCount, su, sv, du4, dv4, tex, texMask, texShift, palBase, rowOff)
    @ r0=rp (set), r1=pairCount, r2=su (set), r3=sv (set)
    @ Stack: du4, dv4, tex, texMask, texShift, palBase, rowOff
    mov     r1, r7                      @ pairCount
    @ r2=su, r3=sv already set

    @ Build stack args for tex_scanline_asm:
    @ [sp+0]=du4, [sp+4]=dv4, [sp+8]=tex, [sp+12]=texMask,
    @ [sp+16]=texShift, [sp+20]=palBase, [sp+24]=rowOff
    sub     sp, sp, #28
    str     r8,  [sp, #0]              @ du4
    str     r11, [sp, #4]              @ dv4
    ldr     r7,  [sp, #28 + TF_TEX]
    str     r7,  [sp, #8]              @ tex
    ldr     r7,  [sp, #28 + TF_TEXMASK]
    str     r7,  [sp, #12]             @ texMask
    ldr     r7,  [sp, #28 + TF_TEXSHIFT]
    str     r7,  [sp, #16]             @ texShift
    ldr     r7,  [sp, #28 + TF_PALBASE]
    str     r7,  [sp, #20]             @ palBase
    mov     r7,  #0
    str     r7,  [sp, #24]             @ rowOff = 0 (no row dup)

    bl      tex_scanline_asm

    add     sp, sp, #28                 @ clean up 7 stack args

.Tscan_advance:
    @ Advance edges using FIQ-banked deltas (saves 6 stack loads per scanline).
    @ Load current UV values into r0-r3 (shared regs, accessible in FIQ mode).
    ldr     r0, [sp, #TF_LU]
    ldr     r1, [sp, #TF_LV]
    ldr     r2, [sp, #TF_RU]
    ldr     r3, [sp, #TF_RV]
    msr     cpsr_c, #0x11          @ FIQ mode — r8-r14 are now the banked deltas
    add     r4, r4, r8             @ Lx += LDX  (r4 shared, r8_fiq = LDX)
    add     r5, r5, r9             @ Rx += RDX  (r5 shared, r9_fiq = RDX)
    add     r0, r0, r10            @ LU += LDU  (r0 shared, r10_fiq = LDU)
    add     r2, r2, r11            @ RU += RDU  (r2 shared, r11_fiq = RDU)
    add     r1, r1, r12            @ LV += LDV  (r1 shared, r12_fiq = LDV)
    add     r3, r3, r14            @ RV += RDV  (r3 shared, r14_fiq = RDV)
    msr     cpsr_c, #0x1F          @ System mode
    @ Store updated UV values
    str     r0, [sp, #TF_LU]
    str     r1, [sp, #TF_LV]
    str     r2, [sp, #TF_RU]
    str     r3, [sp, #TF_RV]
    add     r9, r9, #1
    b       .Tscan_loop

.Tscan_done:
    b       .Tadv_left

.Texit:
    add     sp, sp, #TF_SIZE
    ldmfd   sp!, {r4-r11, lr}
    bx      lr

    .ltorg

.size rasterize_convex_tex_asm, .-rasterize_convex_tex_asm
