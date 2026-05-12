@ mixer_fast.s — ARM assembly sound mixer inner loop for GBA
@ Runs in IWRAM (0 wait-state, ARM mode) for maximum throughput.
@ Processes one voice: mixes waveform data into s16 accumulator buffer.
@ Unrolled 4x with pre-computed volume*gain shift.
@
@ void afn_mix_voice_fast(s16* mix_acc, const s8* wdata, int count,
@                         int pos, int inc, int vol, int gainShift);
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
@   r7  = temp sample value
@   r8  = temp accumulator value
@   r9  = temp sample 2
@   r10 = temp acc 2

    .section .iwram, "ax", %progbits
    .arm
    .align  2
    .global afn_mix_voice_fast
    .type   afn_mix_voice_fast, %function

afn_mix_voice_fast:
    stmfd   sp!, {r4-r10, lr}

    @ Load stack args: pos, inc, vol, gainShift
    @ r0=mix_acc, r1=wdata, r2=count, r3=pos
    ldr     r4, [sp, #32]           @ inc (5th arg, after 4 saved + lr = offset 32)
    ldr     r5, [sp, #36]           @ vol
    ldr     r6, [sp, #40]           @ gainShift

    @ Quick exit if count <= 0 or vol == 0
    cmp     r2, #0
    ble     .Lmix_done
    cmp     r5, #0
    beq     .Lmix_skip_voice

    @ Main loop: process 4 samples per iteration
    subs    r2, r2, #4
    blt     .Lmix_tail

.Lmix_loop4:
    @ Sample 0
    mov     r7, r3, asr #8         @ sampleIdx = pos >> 8
    ldrsb   r7, [r1, r7]           @ wdata[sampleIdx] (signed byte)
    mul     r7, r5, r7             @ sample * vol
    mov     r7, r7, asr r6         @ >> gainShift
    ldrsh   r8, [r0]               @ load current acc
    add     r8, r8, r7             @ accumulate
    strh    r8, [r0], #2           @ store and advance
    add     r3, r3, r4             @ pos += inc

    @ Sample 1
    mov     r7, r3, asr #8
    ldrsb   r7, [r1, r7]
    mul     r7, r5, r7
    mov     r7, r7, asr r6
    ldrsh   r8, [r0]
    add     r8, r8, r7
    strh    r8, [r0], #2
    add     r3, r3, r4

    @ Sample 2
    mov     r7, r3, asr #8
    ldrsb   r7, [r1, r7]
    mul     r7, r5, r7
    mov     r7, r7, asr r6
    ldrsh   r8, [r0]
    add     r8, r8, r7
    strh    r8, [r0], #2
    add     r3, r3, r4

    @ Sample 3
    mov     r7, r3, asr #8
    ldrsb   r7, [r1, r7]
    mul     r7, r5, r7
    mov     r7, r7, asr r6
    ldrsh   r8, [r0]
    add     r8, r8, r7
    strh    r8, [r0], #2
    add     r3, r3, r4

    subs    r2, r2, #4
    bge     .Lmix_loop4

.Lmix_tail:
    @ Handle remaining 0-3 samples
    adds    r2, r2, #4
    beq     .Lmix_done

.Lmix_tail_loop:
    mov     r7, r3, asr #8
    ldrsb   r7, [r1, r7]
    mul     r7, r5, r7
    mov     r7, r7, asr r6
    ldrsh   r8, [r0]
    add     r8, r8, r7
    strh    r8, [r0], #2
    add     r3, r3, r4
    subs    r2, r2, #1
    bne     .Lmix_tail_loop

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
@ afn_mix_clamp_fast — Clamp s16 accumulator to s8 output buffer
@ Processes 4 samples per iteration, packs into 32-bit writes.
@
@ void afn_mix_clamp_fast(s8* buf, const s16* mix_acc, int count);
@ ---------------------------------------------------------------------------
    .global afn_mix_clamp_fast
    .type   afn_mix_clamp_fast, %function

afn_mix_clamp_fast:
    stmfd   sp!, {r4-r7, lr}

    @ r0 = buf (s8*), r1 = mix_acc (s16*), r2 = count
    cmp     r2, #0
    ble     .Lclamp_done

    @ Process 4 at a time
    subs    r2, r2, #4
    blt     .Lclamp_tail

.Lclamp_loop4:
    @ Load 4 s16 values
    ldrsh   r3, [r1], #2
    ldrsh   r4, [r1], #2
    ldrsh   r5, [r1], #2
    ldrsh   r6, [r1], #2

    @ Clamp each to -128..127
    cmp     r3, #127
    movgt   r3, #127
    cmn     r3, #128
    mvnlt   r3, #127              @ r3 = -128

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

    @ Pack 4 bytes into one word: buf[0..3]
    and     r3, r3, #0xFF
    and     r4, r4, #0xFF
    and     r5, r5, #0xFF
    and     r6, r6, #0xFF
    orr     r3, r3, r4, lsl #8
    orr     r3, r3, r5, lsl #16
    orr     r3, r3, r6, lsl #24
    str     r3, [r0], #4

    subs    r2, r2, #4
    bge     .Lclamp_loop4

.Lclamp_tail:
    adds    r2, r2, #4
    beq     .Lclamp_done

.Lclamp_tail_loop:
    ldrsh   r3, [r1], #2
    cmp     r3, #127
    movgt   r3, #127
    cmn     r3, #128
    mvnlt   r3, #127
    strb    r3, [r0], #1
    subs    r2, r2, #1
    bne     .Lclamp_tail_loop

.Lclamp_done:
    ldmfd   sp!, {r4-r7, lr}
    bx      lr

.size afn_mix_clamp_fast, .-afn_mix_clamp_fast
