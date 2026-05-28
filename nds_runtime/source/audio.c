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
// Cap matches NDS hardware — 16 channels. Earlier 8-voice cap mirrored
// GBA's polyphony, but with envelope-driven release tails a busy piano
// over-steals into active sustains. Spending the other 8 channels we
// already have lets notes play out their full release.
// ---------------------------------------------------------------------------
#define SND_MAX_VOICES 16

typedef struct {
    int   handle;        // libnds channel id, or -1 if free
    int   smpIdx;        // sample being played (for vol_scale / pitch-bend follow-up)
    int   channel;       // MIDI channel (for pitch bend mapping)
    int   noteOffTick;   // tick (8.8 fixed) when we should stop, or 0 = let sample finish
    int   note;          // active MIDI note, for pitch bend recompute
    int   baseHz;        // freq we issued (for pitch bend recompute)
    // Envelope state (soft-fade release + decay):
    int   volHead;       // current volume (0..127) — what's on the channel now
    int   volPeak;       // initial volume issued at note-on (decay/release baseline)
    int   ageFrames;     // frames since note-on (for decay countdown)
    int   releaseFrames; // total frames in release ramp (set when releasing)
    int   releaseLeft;   // frames remaining in release; 0 = not releasing
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
// 2^(n/12) in 16.16 fixed, rounded to nearest.
static const uint32_t k_semi_2_16[12] = {
    65536,    69433,    73562,    77937,    82570,    87480,
    92682,    98193,    104031,   110217,   116771,   123716
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
    // First, reclaim any slot whose hardware channel has gone silent
    // since the last audio_tick — audio_tick only runs once per frame
    // (~16ms), and a fast piano can fire several notes in a single frame.
    // Without mid-frame reclaim, slot count drifts and we steal active
    // sustains while channels are already free.
    unsigned int active = soundGetActiveChannels();
    for (int i = 0; i < snd_voice_count; i++) {
        if (snd_voices[i].handle >= 0 &&
            !(active & (1u << snd_voices[i].handle)))
            snd_voices[i].handle = -1;
    }
    for (int i = 0; i < snd_voice_count; i++) {
        if (snd_voices[i].handle < 0) return i;
    }
    // All in use. Prefer voices already in their release ramp (they'd
    // fall to silence soon anyway). If nothing is releasing, refuse to
    // allocate — drop the new note instead of cutting an active sustain.
    int best = -1, bestAge = -1;
    for (int i = 0; i < snd_voice_count; i++) {
        if (snd_voices[i].releaseLeft > 0 && snd_voices[i].ageFrames > bestAge) {
            best = i; bestAge = snd_voices[i].ageFrames;
        }
    }
    if (best < 0) return -1;   // every voice still actively sustaining
    if (snd_voices[best].handle >= 0) soundKill(snd_voices[best].handle);
    snd_voices[best].handle = -1;
    return best;
}

// ---------------------------------------------------------------------------
// Low-level: start a sample on a voice. Used by both the MIDI sequencer
// (afn_audio_tick) and by afn_play_sfx.
// ---------------------------------------------------------------------------
void afn_trigger_sample(int smpIdx, int note, int vel, int durTicks, int ch)
{
    if (smpIdx < 0 || smpIdx >= AFN_SOUND_SAMPLE_COUNT) return;

    int baseHz = afn_pcm_rates[smpIdx];
    int hz = note_to_hz(baseHz, note, 60); // assume baseNote=60; multi-sample regions are baked into 'note' by exporter
#ifdef AFN_HAS_FINE_FACTOR
    // Apply per-sample fineTune as a 16.16 multiplier. The exporter emits
    // factor = round(2^(cents/1200) * 65536), keeping sub-cent precision
    // that the int sampleRate field couldn't carry. Older mapdata.h files
    // without AFN_HAS_FINE_FACTOR fall back to the rate-baked precision.
    {
        unsigned int fine = afn_pcm_fine_factor[smpIdx];
        if (fine != 65536) {
            unsigned long long h = (unsigned long long)hz * fine + 32768ULL;
            hz = (int)(h >> 16);
        }
    }
#endif

    // Volume: combine MIDI velocity (0..127) with per-sample scale (0..255).
    // libnds wants 0..127.
    int scale = afn_pcm_vol_scale[smpIdx];
    int vol = (vel * scale) >> 8;
    if (vol > 127) vol = 127;

    SoundFormat fmt = afn_pcm_is16[smpIdx] ? SoundFormat_16Bit : SoundFormat_8Bit;
    int loop = afn_pcm_loop[smpIdx];
    // NDS hardware SCHANNEL_LENGTH counts words AFTER the loop point, not
    // total. libnds soundPlaySample stuffs dataSize/4 straight into it
    // and passes loopPoint as RAW words. So:
    //   loopPoint = loop_start_samples * bytes_per_sample / 4
    //   dataSize  = (sample_count - loop_start) * bytes_per_sample   (for loop)
    //             = sample_count * bytes_per_sample                  (for one-shot)
    // The earlier "pass total size" version made the hardware play past
    // the end of the sample into the next sample's memory — exactly the
    // rogue extra notes the user heard echoing the real melody.
    int bytesPerSample = afn_pcm_is16[smpIdx] ? 2 : 1;
    int loopStart = loop ? afn_pcm_loop_start[smpIdx] : 0;
    int sampleBytes = (afn_pcm_lens[smpIdx] - loopStart) * bytesPerSample;
    int loopWords   = (loopStart * bytesPerSample) / 4;

    int handle = soundPlaySample(
        (void*)afn_pcm_ptrs[smpIdx],
        fmt,
        sampleBytes,
        hz,
        vol,
        64,                         // pan center
        loop ? true : false,
        loopWords
    );

    // Fire-and-forget for SFX (durTicks <= 0): don't reserve a slot in
    // snd_voices[] — libnds owns the 16 hardware channels and will recycle
    // them when the sample ends. We only track in the voice table when
    // we need pitch-bend or note-off bookkeeping (sequenced MIDI notes).
    // Without this, every collected ring's SFX held a voice slot
    // forever, eventually stealing voice 0 = the MIDI sustain.
    if (durTicks <= 0) return;

    int v = alloc_voice();
    if (v < 0) {
        // Every slot still actively sustaining — drop the new note rather
        // than killing one. The hardware channel we already kicked via
        // soundPlaySample will keep going untracked, but since we won't
        // pitch-bend or release it, that's effectively a fire-and-forget
        // SFX. soundGetActiveChannels-based reclaim picks it up later.
        return;
    }
    SndVoice* vc = &snd_voices[v];
    vc->handle        = handle;
    vc->smpIdx        = smpIdx;
    vc->channel       = ch;
    vc->note          = note;
    vc->baseHz        = baseHz;
    vc->noteOffTick   = (snd_seq_tick >> 8) + durTicks;
    vc->volHead       = vol;
    vc->volPeak       = vol;
    vc->ageFrames     = 0;
    vc->releaseFrames = 0;
    vc->releaseLeft   = 0;
}

// ---------------------------------------------------------------------------
// SFX entry point (called by setActionFunc-emitted PlaySfx node bodies).
// On NDS the 'fifo' arg is a no-op — channels are interchangeable.
// ---------------------------------------------------------------------------
void afn_play_sfx(int smpIdx, int gain, int fifo)
{
    (void)fifo;
    if (gain <= 0) gain = 127;
    // Ensure there's a free hardware channel for the SFX. libnds's
    // soundPlaySample silently fails when all 16 channels are busy —
    // that's how SFX go missing when MIDI BGM is dense. First reclaim
    // any tracked voice whose channel has already gone silent, then if
    // still saturated steal the oldest BGM voice already in release
    // (it would have rung out shortly anyway) so the SFX can play.
    unsigned int active = soundGetActiveChannels();
    for (int i = 0; i < SND_MAX_VOICES; i++) {
        if (snd_voices[i].handle >= 0 &&
            !(active & (1u << snd_voices[i].handle)))
            snd_voices[i].handle = -1;
    }
    if (active == 0xFFFFu) {
        int best = -1, bestAge = -1;
        for (int i = 0; i < SND_MAX_VOICES; i++) {
            if (snd_voices[i].releaseLeft > 0 && snd_voices[i].ageFrames > bestAge) {
                best = i; bestAge = snd_voices[i].ageFrames;
            }
        }
        if (best < 0) {
            for (int i = 0; i < SND_MAX_VOICES; i++) {
                if (snd_voices[i].handle >= 0 && snd_voices[i].ageFrames > bestAge) {
                    best = i; bestAge = snd_voices[i].ageFrames;
                }
            }
        }
        if (best >= 0) {
            soundKill(snd_voices[best].handle);
            snd_voices[best].handle = -1;
        }
    }
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
    // Editor's voiceCount was a GBA CPU-mixing budget; on NDS each voice
    // is a free hardware channel, so override to the full cap unless the
    // editor explicitly asked for less than half. That extends polyphony
    // headroom past what the editor stored and stops aggressive steals
    // from cutting active piano notes during release tails.
    int editorVoices = afn_snd_voices[instanceId];
    snd_voice_count = SND_MAX_VOICES;
    if (editorVoices > 0 && editorVoices < SND_MAX_VOICES / 2)
        snd_voice_count = editorVoices;
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
    // Envelope pass — advance decay while held, ramp during release, kill on
    // release end. Snap-cut only when the instance opts out of soft-fade
    // (afn_snd_soft_fade[instance] == 0) or the sample doesn't declare a
    // release tail. Tick runs at 60 Hz; convert *ms / 16 (≈*60/1000) to
    // frames cheaply.
    int nowTick = snd_seq_tick >> 8;
    int softFade = 1;
    if (snd_seq_active >= 0 && snd_seq_active < AFN_SOUND_INSTANCE_COUNT)
        softFade = afn_snd_soft_fade[snd_seq_active];
    for (int i = 0; i < SND_MAX_VOICES; i++) {
        SndVoice* vc = &snd_voices[i];
        if (vc->handle < 0) continue;
        // Drop voices whose sample played out naturally — libnds has no
        // soundActive() helper, but soundGetActiveChannels() returns a
        // bitmask of channels still running. Without this check drum hits
        // (which finish in ~50ms) keep their slot in snd_voices forever
        // and chew through the 16-voice budget.
        if (!(soundGetActiveChannels() & (1u << vc->handle))) {
            vc->handle = -1;
            continue;
        }
        vc->ageFrames++;

        // Decay (during sustained hold). Sample decays its initial peak by
        // decay_pct over (note duration - decay_min_ms). Computed as a
        // simple linear ramp on volHead.
        if (vc->releaseLeft == 0 && vc->smpIdx >= 0 && vc->smpIdx < AFN_SOUND_SAMPLE_COUNT) {
            int decayPct = afn_pcm_decay_pct[vc->smpIdx];
            int decayMin = afn_pcm_decay_min_ms[vc->smpIdx];
            int decayMinF = (decayMin * 60 + 999) / 1000;
            if (decayPct > 0 && vc->ageFrames > decayMinF) {
                int span = vc->ageFrames - decayMinF;
                // Drop to (1 - decayPct/100) over the next 60 frames (1s).
                int drop = (vc->volPeak * decayPct * span) / (100 * 60);
                int target = vc->volPeak - drop;
                if (target < 0) target = 0;
                if (target != vc->volHead) {
                    vc->volHead = target;
                    soundSetVolume(vc->handle, target);
                }
            }
        }

        // Start release once duration is up.
        if (vc->releaseLeft == 0 && vc->noteOffTick > 0 && nowTick >= vc->noteOffTick) {
            int rms = (vc->smpIdx >= 0 && vc->smpIdx < AFN_SOUND_SAMPLE_COUNT)
                      ? afn_pcm_release_ms[vc->smpIdx] : 0;
            int rframes = (rms * 60 + 999) / 1000;
            if (!softFade) rframes = 0;
            if (rframes <= 0) {
                soundKill(vc->handle);
                vc->handle = -1;
                continue;
            }
            vc->releaseFrames = rframes;
            vc->releaseLeft   = rframes;
        }

        // Tick release ramp.
        if (vc->releaseLeft > 0) {
            vc->releaseLeft--;
            int target = (vc->volHead * vc->releaseLeft) / vc->releaseFrames;
            if (target < 0) target = 0;
            soundSetVolume(vc->handle, target);
            if (vc->releaseLeft == 0) {
                soundKill(vc->handle);
                vc->handle = -1;
            }
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
            // Scale BGM velocity by the editor's master dB so the user can
            // pull MIDI down (or up) at export time without touching SFX.
            // Fallback for older mapdata.h files without the macro is unity.
#ifdef AFN_MIDI_MASTER_VOL_FIX
            int bgmVel = (n->vel * AFN_MIDI_MASTER_VOL_FIX) >> 8;
            if (bgmVel > 127) bgmVel = 127;
#else
            int bgmVel = n->vel;
#endif
            afn_trigger_sample(n->smpIdx, n->note, bgmVel, n->dur, n->channel);
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
            // Voices that were sustaining when the loop wrapped have a
            // noteOffTick relative to the pre-wrap tick — that's now way
            // in the future, so they'd never release on their own. Force
            // each one into its release ramp so old notes ring out
            // properly instead of sustaining forever across the seam.
            for (int i = 0; i < SND_MAX_VOICES; i++) {
                SndVoice* vc = &snd_voices[i];
                if (vc->handle < 0 || vc->releaseLeft > 0) continue;
                int relFrames = 0;
                if (vc->smpIdx >= 0 && vc->smpIdx < AFN_SOUND_SAMPLE_COUNT)
                    relFrames = (afn_pcm_release_ms[vc->smpIdx] * 60 + 999) / 1000;
                if (relFrames < 1) relFrames = 1;
                vc->releaseFrames = relFrames;
                vc->releaseLeft   = relFrames;
                vc->noteOffTick   = snd_seq_tick >> 8;
            }
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
