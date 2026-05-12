@ mixer_fast.s — ARM assembly sound mixer with linear interpolation for GBA
@ Runs in IWRAM (0 wait-state, ARM mode) for maximum throughput.
@ Processes one voice: mixes waveform data into s16 accumulator buffer.
@ Linear interpolation between samples for smooth sustained tones.
@ Samples must have +1 padding byte at end for safe interpolation read.
@
@ int afn_mix_voice_fast(s16* mix_acc, const s8* wdata, int count,
@                        int pos, int inc, int vol, int gainShift);
@ Returns: new pos value (in r0)
@
@ Register plan:
@   r0  = mix_acc ptr (advances)
@   r1  = wdata base ptr
@   r2  = count (samples remaining)
@   r3  = pos (24.8 fixed point)
@   r4  = inc (24.8 fixed point)
@   r5  = vol (0-127)
@   r6  = gainShift
@   r7  = idx (pos >> 8)
@   r8  = s0 (current sample)
@   r9  = s1 (next sample) / frac
@   r10 = temp accumulator / interpolated value

    .section .iwram, "ax", %progbits
    .arm
    .align  2
    .global afn_mix_voice_fast
    .type   afn_mix_voice_fast, %function

afn_mix_voice_fast:
    stmfd   sp!, {r4-r10, lr}

    @ Load stack args: inc, vol, gainShift
    @ r0=mix_acc, r1=wdata, r2=count, r3=pos
    ldr     r4, [sp, #32]           @ inc
    ldr     r5, [sp, #36]           @ vol
    ldr     r6, [sp, #40]           @ gainShift

    @ Quick exit if count <= 0 or vol == 0
    cmp     r2, #0
    ble     .Lmix_done
    cmp     r5, #0
    beq     .Lmix_skip_voice

    @ Main loop: process 2 samples per iteration
    subs    r2, r2, #2
    blt     .Lmix_tail

.Lmix_loop2:
    @ --- Sample 0 ---
    mov     r7, r3, asr #8         @ idx = pos >> 8
    ldrsb   r8, [r1, r7]           @ s0 = wdata[idx]
    add     r7, r7, #1
    ldrsb   r9, [r1, r7]           @ s1 = wdata[idx+1]
    sub     r9, r9, r8             @ s1 - s0
    and     r7, r3, #0xFF          @ frac = pos & 0xFF
    mul     r9, r7, r9             @ (s1-s0) * frac
    add     r8, r8, r9, asr #8    @ s0 + ((s1-s0)*frac >> 8)
    mul     r8, r5, r8             @ * vol
    mov     r8, r8, asr r6         @ >> gainShift
    ldrsh   r10, [r0]              @ load accumulator
    add     r10, r10, r8           @ accumulate
    strh    r10, [r0], #2          @ store and advance
    add     r3, r3, r4             @ pos += inc

    @ --- Sample 1 ---
    mov     r7, r3, asr #8
    ldrsb   r8, [r1, r7]
    add     r7, r7, #1
    ldrsb   r9, [r1, r7]
    sub     r9, r9, r8
    and     r7, r3, #0xFF
    mul     r9, r7, r9
    add     r8, r8, r9, asr #8
    mul     r8, r5, r8
    mov     r8, r8, asr r6
    ldrsh   r10, [r0]
    add     r10, r10, r8
    strh    r10, [r0], #2
    add     r3, r3, r4

    subs    r2, r2, #2
    bge     .Lmix_loop2

.Lmix_tail:
    @ Handle remaining 0-1 samples
    adds    r2, r2, #2
    beq     .Lmix_done

    @ One remaining sample
    mov     r7, r3, asr #8
    ldrsb   r8, [r1, r7]
    add     r7, r7, #1
    ldrsb   r9, [r1, r7]
    sub     r9, r9, r8
    and     r7, r3, #0xFF
    mul     r9, r7, r9
    add     r8, r8, r9, asr #8
    mul     r8, r5, r8
    mov     r8, r8, asr r6
    ldrsh   r10, [r0]
    add     r10, r10, r8
    strh    r10, [r0], #2
    add     r3, r3, r4

.Lmix_done:
    mov     r0, r3                  @ return new pos
    ldmfd   sp!, {r4-r10, lr}
    bx      lr

.Lmix_skip_voice:
    @ vol == 0: just advance pos by count*inc, don't touch accumulator
    mul     r7, r2, r4
    add     r3, r3, r7
    mov     r0, r3
    ldmfd   sp!, {r4-r10, lr}
    bx      lr

.size afn_mix_voice_fast, .-afn_mix_voice_fast

@ ---------------------------------------------------------------------------
@ afn_mix_voice_loop — 8-bit mixer with loop wrapping handled internally
@ One call per voice per frame — no chunk dispatch overhead.
@
@ int afn_mix_voice_loop(s16* mix_acc, const s8* wdata, int count,
@                        int pos, int inc, int vol, int gainShift,
@                        int loopLen, int loopSpan);
@ Returns: new pos value (in r0)
@ ---------------------------------------------------------------------------
    .global afn_mix_voice_loop
    .type   afn_mix_voice_loop, %function

afn_mix_voice_loop:
    stmfd   sp!, {r4-r12, lr}

    @ r0=mix_acc, r1=wdata, r2=count, r3=pos
    @ Stack after 10 regs saved (40 bytes):
    ldr     r4, [sp, #40]           @ inc
    ldr     r5, [sp, #44]           @ vol
    ldr     r6, [sp, #48]           @ gainShift
    ldr     r11, [sp, #52]          @ loopLen
    ldr     r12, [sp, #56]          @ loopSpan

    cmp     r2, #0
    ble     .Lloop_done
    cmp     r5, #0
    beq     .Lloop_skip

.Lloop_main:
    @ --- Mix one sample ---
    mov     r7, r3, asr #8         @ idx = pos >> 8
    ldrsb   r8, [r1, r7]           @ s0 = wdata[idx]
    add     r7, r7, #1
    ldrsb   r9, [r1, r7]           @ s1 = wdata[idx+1]
    sub     r9, r9, r8             @ s1 - s0
    and     r7, r3, #0xFF          @ frac = pos & 0xFF
    mul     r9, r7, r9             @ (s1-s0) * frac
    add     r8, r8, r9, asr #8    @ interpolated sample
    mul     r8, r5, r8             @ * vol
    mov     r8, r8, asr r6         @ >> gainShift
    ldrsh   r10, [r0]
    add     r10, r10, r8
    strh    r10, [r0], #2
    add     r3, r3, r4             @ pos += inc
    @ Wrap loop
1:  cmp     r3, r11
    subge   r3, r3, r12
    bge     1b

    subs    r2, r2, #1
    bgt     .Lloop_main

.Lloop_done:
    mov     r0, r3
    ldmfd   sp!, {r4-r12, lr}
    bx      lr

.Lloop_skip:
    mul     r7, r2, r4
    add     r3, r3, r7
1:  cmp     r3, r11
    subge   r3, r3, r12
    bge     1b
    mov     r0, r3
    ldmfd   sp!, {r4-r12, lr}
    bx      lr

.size afn_mix_voice_loop, .-afn_mix_voice_loop

@ ---------------------------------------------------------------------------
@ afn_mix_voice_loop16 — 16-bit mixer with loop wrapping handled internally
@
@ int afn_mix_voice_loop16(s16* mix_acc, const s16* wdata, int count,
@                          int pos, int inc, int vol, int gainShift,
@                          int loopLen, int loopSpan);
@ Returns: new pos value (in r0)
@ ---------------------------------------------------------------------------
    .global afn_mix_voice_loop16
    .type   afn_mix_voice_loop16, %function

afn_mix_voice_loop16:
    stmfd   sp!, {r4-r12, lr}

    ldr     r4, [sp, #40]           @ inc
    ldr     r5, [sp, #44]           @ vol
    ldr     r6, [sp, #48]           @ gainShift
    ldr     r11, [sp, #52]          @ loopLen
    ldr     r12, [sp, #56]          @ loopSpan

    cmp     r2, #0
    ble     .Lloop16_done
    cmp     r5, #0
    beq     .Lloop16_skip

.Lloop16_main:
    mov     r7, r3, asr #8         @ idx = pos >> 8
    mov     r8, r7, lsl #1         @ byte offset = idx * 2
    ldrsh   r8, [r1, r8]           @ s0 = wdata[idx]
    add     r7, r7, #1
    mov     r9, r7, lsl #1
    ldrsh   r9, [r1, r9]           @ s1 = wdata[idx+1]
    sub     r9, r9, r8
    and     r7, r3, #0xFF          @ frac
    mul     r9, r7, r9
    add     r8, r8, r9, asr #8    @ interpolated
    mul     r8, r5, r8             @ * vol
    mov     r8, r8, asr r6         @ >> gainShift
    ldrsh   r10, [r0]
    add     r10, r10, r8
    strh    r10, [r0], #2
    add     r3, r3, r4             @ pos += inc
    @ Wrap loop
1:  cmp     r3, r11
    subge   r3, r3, r12
    bge     1b

    subs    r2, r2, #1
    bgt     .Lloop16_main

.Lloop16_done:
    mov     r0, r3
    ldmfd   sp!, {r4-r12, lr}
    bx      lr

.Lloop16_skip:
    mul     r7, r2, r4
    add     r3, r3, r7
1:  cmp     r3, r11
    subge   r3, r3, r12
    bge     1b
    mov     r0, r3
    ldmfd   sp!, {r4-r12, lr}
    bx      lr

.size afn_mix_voice_loop16, .-afn_mix_voice_loop16

@ ---------------------------------------------------------------------------
@ afn_mix_voice_fast16 — Same as above but reads s16 sample data
@ Linear interpolation in 16-bit precision for high-quality mixing.
@
@ int afn_mix_voice_fast16(s16* mix_acc, const s16* wdata, int count,
@                          int pos, int inc, int vol, int gainShift);
@ Returns: new pos value (in r0)
@ ---------------------------------------------------------------------------
    .global afn_mix_voice_fast16
    .type   afn_mix_voice_fast16, %function

afn_mix_voice_fast16:
    stmfd   sp!, {r4-r10, lr}

    ldr     r4, [sp, #32]           @ inc
    ldr     r5, [sp, #36]           @ vol
    ldr     r6, [sp, #40]           @ gainShift

    cmp     r2, #0
    ble     .Lmix16_done
    cmp     r5, #0
    beq     .Lmix16_skip_voice

    @ Main loop: 2 samples per iteration
    subs    r2, r2, #2
    blt     .Lmix16_tail

.Lmix16_loop2:
    @ --- Sample 0 ---
    mov     r7, r3, asr #8         @ idx = pos >> 8
    mov     r8, r7, lsl #1         @ byte offset = idx * 2
    ldrsh   r8, [r1, r8]           @ s0 = wdata[idx] (signed halfword)
    add     r7, r7, #1
    mov     r9, r7, lsl #1
    ldrsh   r9, [r1, r9]           @ s1 = wdata[idx+1]
    sub     r9, r9, r8             @ s1 - s0
    and     r7, r3, #0xFF          @ frac = pos & 0xFF
    mul     r9, r7, r9             @ (s1-s0) * frac
    add     r8, r8, r9, asr #8    @ s0 + interpolated
    mul     r8, r5, r8             @ * vol
    mov     r8, r8, asr r6         @ >> gainShift
    ldrsh   r10, [r0]
    add     r10, r10, r8
    strh    r10, [r0], #2
    add     r3, r3, r4             @ pos += inc

    @ --- Sample 1 ---
    mov     r7, r3, asr #8
    mov     r8, r7, lsl #1
    ldrsh   r8, [r1, r8]
    add     r7, r7, #1
    mov     r9, r7, lsl #1
    ldrsh   r9, [r1, r9]
    sub     r9, r9, r8
    and     r7, r3, #0xFF
    mul     r9, r7, r9
    add     r8, r8, r9, asr #8
    mul     r8, r5, r8
    mov     r8, r8, asr r6
    ldrsh   r10, [r0]
    add     r10, r10, r8
    strh    r10, [r0], #2
    add     r3, r3, r4

    subs    r2, r2, #2
    bge     .Lmix16_loop2

.Lmix16_tail:
    adds    r2, r2, #2
    beq     .Lmix16_done

    @ One remaining sample
    mov     r7, r3, asr #8
    mov     r8, r7, lsl #1
    ldrsh   r8, [r1, r8]
    add     r7, r7, #1
    mov     r9, r7, lsl #1
    ldrsh   r9, [r1, r9]
    sub     r9, r9, r8
    and     r7, r3, #0xFF
    mul     r9, r7, r9
    add     r8, r8, r9, asr #8
    mul     r8, r5, r8
    mov     r8, r8, asr r6
    ldrsh   r10, [r0]
    add     r10, r10, r8
    strh    r10, [r0], #2
    add     r3, r3, r4

.Lmix16_done:
    mov     r0, r3
    ldmfd   sp!, {r4-r10, lr}
    bx      lr

.Lmix16_skip_voice:
    mul     r7, r2, r4
    add     r3, r3, r7
    mov     r0, r3
    ldmfd   sp!, {r4-r10, lr}
    bx      lr

.size afn_mix_voice_fast16, .-afn_mix_voice_fast16

@ ---------------------------------------------------------------------------
@ afn_mix_clamp_fast — Shift s16 accumulator right, then clamp to s8 output
@ Processes 4 samples per iteration, packs into 32-bit writes.
@ The shift allows mixing at full per-voice precision, scaling once at output.
@
@ void afn_mix_clamp_fast(s8* buf, const s16* mix_acc, int count, int shift);
@ ---------------------------------------------------------------------------
    .global afn_mix_clamp_fast
    .type   afn_mix_clamp_fast, %function

afn_mix_clamp_fast:
    stmfd   sp!, {r4-r7, lr}

    @ r0 = buf (s8*), r1 = mix_acc (s16*), r2 = count, r3 = shift
    cmp     r2, #0
    ble     .Lclamp_done

    @ Process 4 at a time
    subs    r2, r2, #4
    blt     .Lclamp_tail

.Lclamp_loop4:
    @ Load 4 s16 values and shift right
    ldrsh   r4, [r1], #2
    ldrsh   r5, [r1], #2
    ldrsh   r6, [r1], #2
    ldrsh   r7, [r1], #2
    mov     r4, r4, asr r3
    mov     r5, r5, asr r3
    mov     r6, r6, asr r3
    mov     r7, r7, asr r3

    @ Clamp each to -128..127
    cmp     r4, #127
    movgt   r4, #127
    cmn     r4, #128
    mvnlt   r4, #127

    cmp     r5, #127
    movgt   r5, #127
    cmn     r5, #128
    mvnlt   r5, #127

    cmp     r6, #127
    movgt   r6, #127
    cmn     r6, #128
    mvnlt   r6, #127

    cmp     r7, #127
    movgt   r7, #127
    cmn     r7, #128
    mvnlt   r7, #127

    @ Pack 4 bytes into one word: buf[0..3]
    and     r4, r4, #0xFF
    and     r5, r5, #0xFF
    and     r6, r6, #0xFF
    and     r7, r7, #0xFF
    orr     r4, r4, r5, lsl #8
    orr     r4, r4, r6, lsl #16
    orr     r4, r4, r7, lsl #24
    str     r4, [r0], #4

    subs    r2, r2, #4
    bge     .Lclamp_loop4

.Lclamp_tail:
    adds    r2, r2, #4
    beq     .Lclamp_done

.Lclamp_tail_loop:
    ldrsh   r4, [r1], #2
    mov     r4, r4, asr r3
    cmp     r4, #127
    movgt   r4, #127
    cmn     r4, #128
    mvnlt   r4, #127
    strb    r4, [r0], #1
    subs    r2, r2, #1
    bne     .Lclamp_tail_loop

.Lclamp_done:
    ldmfd   sp!, {r4-r7, lr}
    bx      lr

.size afn_mix_clamp_fast, .-afn_mix_clamp_fast
