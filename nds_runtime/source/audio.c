// Affinity NDS — audio (Phase 1).
//
// Replaces the entire GBA mixer (mixer_fast.s + ~800 lines of chunked-mix,
// Timer1 IRQ, FIFO A/B DMA, __isr_table hack) with libnds calls. ARM7 owns
// audio; ARM9 just schedules notes.

#include "affinity.h"
#include "mapdata.h"
#include <nds/arm9/sound.h>

#ifndef AFN_HAS_SOUND
// Project has no sound — keep symbols defined so script bodies / main.c link.
void afn_audio_init(void) {}
void afn_audio_tick(void) {}
void afn_play_sound(int id) { (void)id; }
void afn_stop_sound(void) {}
void afn_play_sfx(int smpIdx, int gain, int fifo) { (void)smpIdx; (void)gain; (void)fifo; }
void afn_trigger_sample(int smpIdx, int note, int vel, int dur, int ch) {
    (void)smpIdx; (void)note; (void)vel; (void)dur; (void)ch;
}

int snd_seq_active = -1;
int snd_seq_tick = 0;
int snd_seq_next = 0;
#else

// ---------------------------------------------------------------------------
// Voice table — mirrors GBA's snd_voices but each slot is just an NDS channel
// handle plus enough state to track note-off + pitch bend follow-up.
// We cap at SND_MAX_VOICES even though hardware has 16, so project polyphony
// numbers from the editor mean the same thing on both targets.
// ---------------------------------------------------------------------------
#define SND_MAX_VOICES 8

typedef struct {
    int   handle;        // libnds channel id, or -1 if free
    int   smpIdx;        // sample being played (for vol_scale / pitch-bend follow-up)
    int   channel;       // MIDI channel (for pitch bend mapping)
    int   noteOffTick;   // tick (8.8 fixed) when we should stop, or 0 = let sample finish
    int   note;          // active MIDI note, for pitch bend recompute
    int   baseHz;        // freq we issued (for pitch bend recompute)
} SndVoice;

static SndVoice snd_voices[SND_MAX_VOICES];
static int snd_voice_count = SND_MAX_VOICES;

// MIDI sequencer state (these names are what setActionFunc bodies reference)
int snd_seq_active = -1;     // instanceId of currently playing song, or -1
int snd_seq_tick   = 0;      // 8.8 fixed-point ticks elapsed
int snd_seq_next   = 0;      // next note index in afn_snd_note_ptrs[snd_seq_active]
static int snd_pitch_bend[16];  // per-channel bend (-8192..+8191)

// ---------------------------------------------------------------------------
// Note → frequency. Standard equal-temperament: freq = base * 2^((note-base)/12).
// 12-entry table of 2^(n/12) in 16.16 fixed. We index by (note-base) % 12 and
// shift by octaves to handle the rest. Faster than pow() and avoids float.
// ---------------------------------------------------------------------------
static const uint32_t k_semi_2_16[12] = {
    65536,    69433,    73562,    77936,    82570,    87480,
    92682,    98193,    104032,   110218,   116772,   123715
};

static int note_to_hz(int baseHz, int noteFrom60Like, int baseNote)
{
    int rel = noteFrom60Like - baseNote;
    int oct = rel / 12;
    int sem = rel - oct * 12;
    if (sem < 0) { sem += 12; oct -= 1; }
    uint64_t hz = (uint64_t)baseHz * k_semi_2_16[sem] >> 16;
    if (oct > 0) hz <<= oct;
    else if (oct < 0) hz >>= -oct;
    if (hz < 1) hz = 1;
    if (hz > 0x7FFFFFFF) hz = 0x7FFFFFFF;
    return (int)hz;
}

// ---------------------------------------------------------------------------
// Pick a free voice, or steal the oldest playing one if all busy.
// ---------------------------------------------------------------------------
static int alloc_voice(void)
{
    for (int i = 0; i < snd_voice_count; i++) {
        if (snd_voices[i].handle < 0) return i;
    }
    // All in use: steal voice 0 (simplest; GBA does similar with a round-robin
    // — refine later if it becomes audible).
    if (snd_voices[0].handle >= 0) soundKill(snd_voices[0].handle);
    snd_voices[0].handle = -1;
    return 0;
}

// ---------------------------------------------------------------------------
// Low-level: start a sample on a voice. Used by both the MIDI sequencer
// (afn_audio_tick) and by afn_play_sfx.
// ---------------------------------------------------------------------------
void afn_trigger_sample(int smpIdx, int note, int vel, int durTicks, int ch)
{
    if (smpIdx < 0 || smpIdx >= AFN_SOUND_SAMPLE_COUNT) return;

    int v = alloc_voice();
    SndVoice* vc = &snd_voices[v];

    int baseHz = afn_pcm_rates[smpIdx];
    int hz = note_to_hz(baseHz, note, 60); // assume baseNote=60; multi-sample regions are baked into 'note' by exporter

    // Volume: combine MIDI velocity (0..127) with per-sample scale (0..255).
    // libnds wants 0..127.
    int scale = afn_pcm_vol_scale[smpIdx];
    int vol = (vel * scale) >> 8;
    if (vol > 127) vol = 127;

    SoundFormat fmt = afn_pcm_is16[smpIdx] ? SoundFormat_16Bit : SoundFormat_8Bit;
    int loop = afn_pcm_loop[smpIdx];
    int loopStart = afn_pcm_loop_start[smpIdx];
    int sampleSize = afn_pcm_lens[smpIdx];

    int handle = soundPlaySample(
        (void*)afn_pcm_ptrs[smpIdx],
        fmt,
        sampleSize,
        hz,
        vol,
        64,                         // pan center
        loop ? true : false,
        loopStart
    );

    vc->handle      = handle;
    vc->smpIdx      = smpIdx;
    vc->channel     = ch;
    vc->note        = note;
    vc->baseHz      = baseHz;
    vc->noteOffTick = (durTicks > 0) ? ((snd_seq_tick >> 8) + durTicks) : 0;
}

// ---------------------------------------------------------------------------
// SFX entry point (called by setActionFunc-emitted PlaySfx node bodies).
// On NDS the 'fifo' arg is a no-op — channels are interchangeable.
// ---------------------------------------------------------------------------
void afn_play_sfx(int smpIdx, int gain, int fifo)
{
    (void)fifo;
    if (gain <= 0) gain = 127;
    afn_trigger_sample(smpIdx, 60, gain, 0, 15);
}

// ---------------------------------------------------------------------------
// Start an instance's MIDI sequence.
// ---------------------------------------------------------------------------
void afn_play_sound(int instanceId)
{
    if (instanceId < 0 || instanceId >= AFN_SOUND_INSTANCE_COUNT) return;
    if (!afn_snd_note_ptrs[instanceId] || afn_snd_note_counts[instanceId] == 0) return;
    snd_seq_active = instanceId;
    snd_seq_tick   = 0;
    snd_seq_next   = 0;
    for (int i = 0; i < 16; i++) snd_pitch_bend[i] = 0;
    snd_voice_count = afn_snd_voices[instanceId];
    if (snd_voice_count < 4) snd_voice_count = 4;
    if (snd_voice_count > SND_MAX_VOICES) snd_voice_count = SND_MAX_VOICES;
}

// ---------------------------------------------------------------------------
// Stop everything (called by StopSound node body).
// ---------------------------------------------------------------------------
void afn_stop_sound(void)
{
    snd_seq_active = -1;
    for (int i = 0; i < SND_MAX_VOICES; i++) {
        if (snd_voices[i].handle >= 0) soundKill(snd_voices[i].handle);
        snd_voices[i].handle = -1;
    }
}

// ---------------------------------------------------------------------------
// Hardware bring-up — once at boot.
// ---------------------------------------------------------------------------
void afn_audio_init(void)
{
    soundEnable();
    for (int i = 0; i < SND_MAX_VOICES; i++) snd_voices[i].handle = -1;
}

// ---------------------------------------------------------------------------
// Per-VBlank: advance the sequence, fire note-on / note-off, apply pitch bend.
// ---------------------------------------------------------------------------
void afn_audio_tick(void)
{
    // Note-off pass — kill voices whose duration is up.
    int nowTick = snd_seq_tick >> 8;
    for (int i = 0; i < SND_MAX_VOICES; i++) {
        SndVoice* vc = &snd_voices[i];
        if (vc->handle < 0) continue;
        if (vc->noteOffTick > 0 && nowTick >= vc->noteOffTick) {
            soundKill(vc->handle);
            vc->handle = -1;
        }
    }

    if (snd_seq_active < 0 || snd_seq_active >= AFN_SOUND_INSTANCE_COUNT) return;

    const AfnSndNote* notes = afn_snd_note_ptrs[snd_seq_active];
    int count = afn_snd_note_counts[snd_seq_active];
    int tpf = afn_snd_tpf[snd_seq_active];
    if (!notes || count == 0) { snd_seq_active = -1; return; }

    snd_seq_tick += tpf;

    // Fire any notes whose tick has arrived.
    while (snd_seq_next < count && (notes[snd_seq_next].tick << 8) <= snd_seq_tick) {
        const AfnSndNote* n = &notes[snd_seq_next];
        if (n->smpIdx == 255) {
            // Pitch bend event: decode value from note(hi) + vel(lo)
            int bv = (n->note << 7) | n->vel;
            snd_pitch_bend[n->channel] = bv - 8192;
            // Recompute freq on any voice for that channel
            for (int i = 0; i < SND_MAX_VOICES; i++) {
                SndVoice* vc = &snd_voices[i];
                if (vc->handle < 0 || vc->channel != n->channel) continue;
                // Bend ±2 semitones; map to a multiplier in 8.8 fixed
                int adj = 256 + (snd_pitch_bend[n->channel] * 256) / 49152;
                if (adj < 128) adj = 128;
                int hz = (vc->baseHz * adj) >> 8;
                hz = note_to_hz(hz, vc->note, 60);
                soundSetFreq(vc->handle, hz);
            }
        } else {
            afn_trigger_sample(n->smpIdx, n->note, n->vel, n->dur, n->channel);
        }
        snd_seq_next++;
    }

    // Loop handling — same semantics as GBA's afn_sound_tick.
    if (afn_snd_loop[snd_seq_active]) {
        int loopEnd = afn_snd_loop_end[snd_seq_active];
        if (loopEnd > 0 && nowTick >= loopEnd) {
            int loopStart = afn_snd_loop_start[snd_seq_active];
            snd_seq_tick = loopStart << 8;
            snd_seq_next = 0;
            while (snd_seq_next < count && notes[snd_seq_next].tick < loopStart)
                snd_seq_next++;
            for (int i = 0; i < 16; i++) snd_pitch_bend[i] = 0;
        }
    } else if (snd_seq_next >= count) {
        // Wait for tails to finish before clearing the active slot — once all
        // hardware channels report idle, mark sequence done.
        int anyActive = 0;
        for (int i = 0; i < SND_MAX_VOICES; i++) {
            if (snd_voices[i].handle >= 0) { anyActive = 1; break; }
        }
        if (!anyActive) snd_seq_active = -1;
    }
}

#endif // AFN_HAS_SOUND
