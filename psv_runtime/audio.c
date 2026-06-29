// Affinity PS Vita runtime — audio: software mixer + MIDI/SFX sequencer.
// Ported from psp_runtime/source/audio.c (which itself ports the NDS sequencer/
// envelope/voice-allocation logic). The mixer and sequencer are IDENTICAL to the
// PSP; only the Sony audio + thread + semaphore APIs differ (pspaudio ->
// sceAudioOut, pspthreadman -> psp2/kernel/threadmgr). The voice table is shared
// between the main thread (afn_audio_tick @ 60 Hz + afn_play_*) and the mixer
// thread, guarded by a semaphore. The data (psv_sound.h) is byte-identical to
// psp_sound.h.
#include "psv_sound.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/audioout.h>
#include <string.h>

#ifndef AFN_HAS_SOUND
// Project has no audio — keep the entry points defined so main.c / script link.
void afn_audio_init(void) {}
void afn_audio_tick(void) {}
void afn_play_sound(int id, int link) { (void)id; (void)link; }
void afn_play_sfx(int s, int g, int f) { (void)s; (void)g; (void)f; }
int  afn_play_sfx_inst_gain(int i, int g) { (void)i; (void)g; return -1; }
void afn_stop_sfx_inst(int i) { (void)i; }
int  afn_sfx_active_inst(int i) { (void)i; return 0; }
void afn_set_sfx_pitch_inst(int i, int p) { (void)i; (void)p; }
int  afn_inst_voice_active(int v, int i) { (void)v; (void)i; return 0; }
void afn_stop_inst_voice(int v, int i) { (void)v; (void)i; }
void afn_stop_sound(void) {}
void afn_stop_all(void) {}
void afn_stop_sfx_sample(int s) { (void)s; }
void afn_stop_music(void) {}
void afn_set_sfx_pitch(int s, int p) { (void)s; (void)p; }
int  afn_sfx_active(int s) { (void)s; return 0; }
#else

// ---- Output config --------------------------------------------------------
#define OUT_RATE     44100          // a Vita-supported sceAudioOut rate
#define AUDIO_FRAMES 1024           // stereo frames per output buffer (64-aligned)
#define SND_MAX_VOICES 16

// ---- Software voice -------------------------------------------------------
typedef struct {
    int          playing;
    const void*  data;
    int          is16;
    int          len;
    int          loop;
    int          loopStart;
    int          pos;
    unsigned int frac;
    unsigned int phaseInc;
    int          vol;
    int smpIdx, channel, note, baseHz;
    int noteOffTick;
    int volPeak, ageFrames, releaseFrames, releaseLeft, isSfxLoop;
} SndVoice;

static SndVoice snd_voices[SND_MAX_VOICES];
static int snd_voice_count = SND_MAX_VOICES;

static int snd_seq_active = -1;
static int snd_seq_tick   = 0;
static int snd_seq_next   = 0;
static int afn_persist_sfx_smp = -1;   // sample index of the persistent music-SFX voice, -1 = none
// --- Linked persistence -----------------------------------------------------
// A Play Sound node with a "Persist Link" > 0 keeps its track alive across scene
// changes, but only between scenes that play it with the SAME link value. Playing
// a persistent sound with a DIFFERENT link stops the old one and starts the new.
static int afn_persist_link = 0;       // link group of the currently-held music, 0 = none
static int afn_persist_inst = -1;      // sound-instance id of the held music, -1 = none
static int snd_pitch_bend[16];

static SceUID s_mutex = -1;
static int    s_port  = -1;
static volatile int s_running = 0;
static short  s_outbuf[2][AUDIO_FRAMES * 2] __attribute__((aligned(64)));
static int    s_acc[AUDIO_FRAMES * 2];

static inline void lock(void)   { if (s_mutex >= 0) sceKernelWaitSema(s_mutex, 1, 0); }
static inline void unlock(void) { if (s_mutex >= 0) sceKernelSignalSema(s_mutex, 1); }

static const unsigned int k_semi_2_16[12] = {
    65536, 69433, 73562, 77937, 82570, 87480,
    92682, 98193, 104031, 110217, 116771, 123716
};
static int note_to_hz(int baseHz, int note, int baseNote) {
    int rel = note - baseNote;
    int oct = rel / 12;
    int sem = rel - oct * 12;
    if (sem < 0) { sem += 12; oct -= 1; }
    unsigned long long hz = (unsigned long long)baseHz * k_semi_2_16[sem] >> 16;
    if (oct > 0) hz <<= oct; else if (oct < 0) hz >>= -oct;
    if (hz < 1) hz = 1;
    if (hz > 0x7FFFFFFF) hz = 0x7FFFFFFF;
    return (int)hz;
}
static inline unsigned int hz_to_inc(int hz) {
    return (unsigned int)(((unsigned long long)hz << 16) / OUT_RATE);
}

static unsigned int active_mask(void) {
    unsigned int m = 0;
    for (int i = 0; i < snd_voice_count; i++)
        if (snd_voices[i].playing) m |= (1u << i);
    return m;
}

static int alloc_voice(void) {
    for (int i = 0; i < snd_voice_count; i++)
        if (!snd_voices[i].playing) return i;
    int best = -1, bestAge = -1;
    for (int i = 0; i < snd_voice_count; i++)
        if (snd_voices[i].releaseLeft > 0 && snd_voices[i].ageFrames > bestAge) {
            best = i; bestAge = snd_voices[i].ageFrames;
        }
    if (best < 0) return -1;
    snd_voices[best].playing = 0;
    return best;
}

static int trigger_sample(int smpIdx, int note, int vel, int durTicks, int ch) {
    if (smpIdx < 0 || smpIdx >= AFN_SOUND_SAMPLE_COUNT) return -1;
    int baseHz = afn_pcm_rates[smpIdx];
    int hz = note_to_hz(baseHz, note, 60);
#ifdef AFN_HAS_FINE_FACTOR
    {
        unsigned int fine = afn_pcm_fine_factor[smpIdx];
        if (fine != 65536) {
            unsigned long long h = (unsigned long long)hz * fine + 32768ULL;
            hz = (int)(h >> 16);
        }
    }
#endif
    int scale = afn_pcm_vol_scale[smpIdx];
    int vol = (vel * scale) >> 8;
    if (vol > 127) vol = 127;
    if (vol < 0) vol = 0;
    int loop = afn_pcm_loop[smpIdx];
    int v = alloc_voice();
    if (v < 0) return -1;
    SndVoice* vc = &snd_voices[v];
    vc->data      = afn_pcm_ptrs[smpIdx];
    vc->is16      = afn_pcm_is16[smpIdx];
    vc->len       = afn_pcm_lens[smpIdx];
    vc->loop      = loop;
    vc->loopStart = loop ? afn_pcm_loop_start[smpIdx] : 0;
    vc->pos       = 0;
    vc->frac      = 0;
    vc->phaseInc  = hz_to_inc(hz);
    vc->vol       = vol;
    vc->smpIdx        = smpIdx;
    vc->channel       = ch;
    vc->note          = note;
    vc->baseHz        = baseHz;
    vc->noteOffTick   = (durTicks > 0) ? (snd_seq_tick >> 8) + durTicks : 0;
    vc->volPeak       = vol;
    vc->ageFrames     = 0;
    vc->releaseFrames = 0;
    vc->releaseLeft   = 0;
    vc->isSfxLoop     = (durTicks <= 0 && loop) ? 1 : 0;
    vc->playing       = 1;
    return v;
}

static int play_sfx_locked(int smpIdx, int gain, int fifo) {
    (void)fifo;
    if (gain <= 0) gain = 127;
    unsigned int active = active_mask();
    if (active == ((1u << snd_voice_count) - 1)) {
        int best = -1, bestAge = -1;
        // Steal a releasing NON-LOOP voice first, then the oldest non-loop voice —
        // never cut a looping SFX (e.g. the enemy charge wind-up) to fit a one-shot.
        for (int i = 0; i < snd_voice_count; i++)
            if (!snd_voices[i].isSfxLoop && snd_voices[i].releaseLeft > 0 && snd_voices[i].ageFrames > bestAge) { best = i; bestAge = snd_voices[i].ageFrames; }
        if (best < 0)
            for (int i = 0; i < snd_voice_count; i++)
                if (!snd_voices[i].isSfxLoop && snd_voices[i].ageFrames > bestAge) { best = i; bestAge = snd_voices[i].ageFrames; }
        // NEVER steal a looping SFX (e.g. the enemy charge): if every voice is a
        // loop, drop THIS one-shot rather than cut the loop. (best stays -1.)
        if (best >= 0) snd_voices[best].playing = 0;
    }
    return trigger_sample(smpIdx, 60, gain, 0, 15);
}

void afn_play_sfx(int smpIdx, int gain, int fifo) {
    lock();
    play_sfx_locked(smpIdx, gain, fifo);
    unlock();
}

// Play SFX sound INSTANCE `inst` at a custom gain (1..127, 0 = full). Resolves the
// instance's sample/fifo from the table — used for proximity-attenuated enemy SFX.
// Returns the voice index it started on (-1 if dropped) so callers can later stop
// THAT voice specifically (needed when two entities share one looping instance).
int afn_play_sfx_inst_gain(int inst, int gain) {
    if (inst < 0 || inst >= AFN_SOUND_INSTANCE_COUNT) return -1;
    if (!afn_snd_is_sfx[inst]) return -1;
    lock();
    int v = play_sfx_locked(afn_snd_sfx_sample[inst], gain, afn_snd_sfx_fifo[inst]);
    unlock();
    return v;
}

// Voice-specific helpers: distinguish two voices of the SAME sample (e.g. the enemy
// charge vs the player's chargefocus — both instance 4). Operate only on voice `v`
// and only while it's still that instance's sample (guards against voice reuse).
int afn_inst_voice_active(int v, int inst) {
    if (v < 0 || v >= SND_MAX_VOICES || inst < 0 || inst >= AFN_SOUND_INSTANCE_COUNT) return 0;
    int r;
    lock();
    r = (snd_voices[v].playing && snd_voices[v].smpIdx == afn_snd_sfx_sample[inst]) ? 1 : 0;
    unlock();
    return r;
}
void afn_stop_inst_voice(int v, int inst) {
    if (v < 0 || v >= SND_MAX_VOICES || inst < 0 || inst >= AFN_SOUND_INSTANCE_COUNT) return;
    lock();
    if (snd_voices[v].playing && snd_voices[v].smpIdx == afn_snd_sfx_sample[inst])
        snd_voices[v].playing = 0;
    unlock();
}

// Stop all voices of SFX instance `inst`'s sample — for looping SFX (e.g. the enemy
// charge wind-up) that must be cut when the action ends.
void afn_stop_sfx_sample(int smpIdx);   // fwd (defined below)
void afn_stop_sfx_inst(int inst) {
    if (inst < 0 || inst >= AFN_SOUND_INSTANCE_COUNT) return;
    if (!afn_snd_is_sfx[inst]) return;
    afn_stop_sfx_sample(afn_snd_sfx_sample[inst]);
}

// Is any voice currently playing SFX instance `inst`'s sample? Used to keep a
// looping SFX (enemy charge) re-asserted each frame if it ever gets cut.
int afn_sfx_active(int smpIdx);   // fwd (defined below)
int afn_sfx_active_inst(int inst) {
    if (inst < 0 || inst >= AFN_SOUND_INSTANCE_COUNT) return 0;
    if (!afn_snd_is_sfx[inst]) return 0;
    return afn_sfx_active(afn_snd_sfx_sample[inst]);
}

// Pitch an SFX INSTANCE's sample (resolve instance -> sample). The AFN_SND_* defines
// are instance ids, NOT sample ids — passing them straight to the sample-indexed
// calls plays the wrong sound once the exporter compacts samples (inst != sample).
void afn_set_sfx_pitch(int smpIdx, int pitch);   // fwd (defined below)
void afn_set_sfx_pitch_inst(int inst, int pitch) {
    if (inst < 0 || inst >= AFN_SOUND_INSTANCE_COUNT) return;
    if (!afn_snd_is_sfx[inst]) return;
    afn_set_sfx_pitch(afn_snd_sfx_sample[inst], pitch);
}

// Start a sequencer (MIDI) instance. Caller holds the lock.
static void start_seq_locked(int instanceId) {
    if (!(afn_snd_note_ptrs[instanceId] && afn_snd_note_counts[instanceId] > 0)) return;
    snd_seq_active = instanceId;
    snd_seq_tick   = 0;
    snd_seq_next   = 0;
    for (int i = 0; i < 16; i++) snd_pitch_bend[i] = 0;
    // Always use the FULL voice pool. The mixer already processes all SND_MAX_VOICES
    // every buffer, so capping to the BGM's editor voice count saved no CPU and only
    // starved combat SFX — the charge/struggle loops and overlapping player/enemy
    // attack sounds were stealing each other's voices. (afn_snd_voices is editor-only.)
    snd_voice_count = SND_MAX_VOICES;
}

// Stop the currently-held linked-persistent track (SFX voice or sequencer) and
// clear the persist slot. Caller holds the lock.
static void afn_stop_persist_locked(void) {
    if (afn_persist_link == 0) return;
    if (afn_persist_sfx_smp >= 0) {
        for (int i = 0; i < SND_MAX_VOICES; i++)
            if (snd_voices[i].smpIdx == afn_persist_sfx_smp) snd_voices[i].playing = 0;
    } else if (afn_persist_inst >= 0 && snd_seq_active == afn_persist_inst) {
        snd_seq_active = -1;
    }
    afn_persist_link    = 0;
    afn_persist_inst    = -1;
    afn_persist_sfx_smp = -1;
}

// link == 0: normal, non-persistent play (stops on the next scene change).
// link  > 0: persistent music. It carries seamlessly across scene changes, but
//            only between scenes that play it with the SAME link. Playing a
//            persistent sound with a DIFFERENT link stops the held one first.
void afn_play_sound(int instanceId, int link) {
    if (instanceId < 0 || instanceId >= AFN_SOUND_INSTANCE_COUNT) return;
    lock();
    int isSfx = afn_snd_is_sfx[instanceId];
    int smp   = isSfx ? afn_snd_sfx_sample[instanceId] : -1;

    if (link > 0) {
        // Same group + same track already playing -> carry, don't restart.
        if (link == afn_persist_link && instanceId == afn_persist_inst) {
            int playing = 0;
            if (isSfx) {
                for (int i = 0; i < SND_MAX_VOICES; i++)
                    if (snd_voices[i].playing && snd_voices[i].smpIdx == smp) { playing = 1; break; }
            } else {
                playing = (snd_seq_active == instanceId);
            }
            if (playing) { unlock(); return; }
        }
        // Different group/track (or not currently playing): swap the held music.
        afn_stop_persist_locked();
        if (isSfx) {
            play_sfx_locked(smp, afn_snd_sfx_gain[instanceId], afn_snd_sfx_fifo[instanceId]);
            afn_persist_sfx_smp = smp;
        } else {
            start_seq_locked(instanceId);
            afn_persist_sfx_smp = -1;
        }
        afn_persist_link = link;
        afn_persist_inst = instanceId;
        unlock();
        return;
    }

    if (isSfx) {
        play_sfx_locked(smp, afn_snd_sfx_gain[instanceId], afn_snd_sfx_fifo[instanceId]);
    } else {
        start_seq_locked(instanceId);
    }
    unlock();
}

void afn_stop_sound(void) {
    lock();
    // A linked-persistent track is held: keep it alive across the scene change.
    if (afn_persist_link != 0) {
        if (afn_persist_sfx_smp >= 0) {
            // Music is a looping SFX voice: stop the sequencer + every other voice.
            snd_seq_active = -1;
            for (int i = 0; i < SND_MAX_VOICES; i++)
                if (snd_voices[i].smpIdx != afn_persist_sfx_smp) snd_voices[i].playing = 0;
        }
        // else: music is the sequencer itself -> leave everything running.
        unlock();
        return;
    }
    snd_seq_active = -1;
    for (int i = 0; i < SND_MAX_VOICES; i++) snd_voices[i].playing = 0;
    unlock();
}

// A single voice that a SAMPLE-WIDE stop must NOT cut. Shields the enemy's
// voice-tracked charge wind-up from the PLAYER's chargefocus StopSound — both
// share sample 4, so the player firing (afn_stop_sfx_sample(4)) used to also kill
// the enemy's charge loop, which then audibly re-started. The enemy stops its OWN
// voice via afn_stop_inst_voice; set this = that voice while it charges. -1 = none.
int afn_sfx_protect_voice = -1;

void afn_stop_sfx_sample(int smpIdx) {
    lock();
    for (int i = 0; i < SND_MAX_VOICES; i++)
        if (i != afn_sfx_protect_voice && snd_voices[i].playing && snd_voices[i].smpIdx == smpIdx)
            snd_voices[i].playing = 0;
    unlock();
}

// Stop every currently-playing LOOPING SFX voice (e.g. the player's hold-to-charge
// sound) without touching one-shots (UI beeps) or persistent music. Used by the Lock
// Player Functions node on menu/game-over screens: a held/looping ability sound that
// got (re)started by a key the menu nav shares can't be separated by an input mask, so
// the runtime just silences the loops while locked. Respects afn_sfx_protect_voice.
void afn_stop_looping_sfx(void) {
    lock();
    for (int i = 0; i < SND_MAX_VOICES; i++)
        if (i != afn_sfx_protect_voice && snd_voices[i].playing && snd_voices[i].isSfxLoop)
            snd_voices[i].playing = 0;
    unlock();
}

// Stop ONLY the linked-persistent track (battle music) and clear the persist
// slot, leaving one-shot SFX (e.g. the clash 'shoot' blast) ringing. Note the
// other two don't fit a clash resolve: afn_stop_all also kills those SFX, and
// afn_stop_sound PRESERVES the music (keeps it alive across scene changes).
void afn_stop_music(void) {
    lock();
    afn_stop_persist_locked();
    unlock();
}

// True if any voice is currently playing this sample (e.g. to keep a non-loop
// SFX going by re-triggering it when it ends).
int afn_sfx_active(int smpIdx) {
    int r = 0;
    lock();
    for (int i = 0; i < SND_MAX_VOICES; i++)
        if (snd_voices[i].playing && snd_voices[i].smpIdx == smpIdx) { r = 1; break; }
    unlock();
    return r;
}

// Repitch every voice currently playing this sample. pitchPct: 100 = natural,
// 200 = +1 octave (2x speed), 50 = -1 octave. Used to ride a looping SFX's pitch
// (e.g. the clash mash sound rising/falling with the struggle balance).
void afn_set_sfx_pitch(int smpIdx, int pitchPct) {
    if (pitchPct < 1) pitchPct = 1;
    lock();
    for (int i = 0; i < SND_MAX_VOICES; i++)
        if (snd_voices[i].playing && snd_voices[i].smpIdx == smpIdx) {
            int hz = (int)(((long long)snd_voices[i].baseHz * pitchPct) / 100);
            if (hz < 1) hz = 1;
            snd_voices[i].phaseInc = hz_to_inc(hz);
        }
    unlock();
}

// Hard stop: silence the sequencer AND every voice, and clear the persist slot —
// unlike afn_stop_sound (which keeps a linked-persistent track alive across scene
// changes). Used to kill battle music outright before the victory jingle.
void afn_stop_all(void) {
    lock();
    snd_seq_active = -1;
    for (int i = 0; i < SND_MAX_VOICES; i++) snd_voices[i].playing = 0;
    afn_persist_link = 0; afn_persist_inst = -1; afn_persist_sfx_smp = -1;
    unlock();
}

void afn_audio_tick(void) {
    lock();
    int nowTick = snd_seq_tick >> 8;
    int softFade = 1;
    if (snd_seq_active >= 0 && snd_seq_active < AFN_SOUND_INSTANCE_COUNT)
        softFade = afn_snd_soft_fade[snd_seq_active];

    for (int i = 0; i < SND_MAX_VOICES; i++) {
        SndVoice* vc = &snd_voices[i];
        if (!vc->playing) continue;
        vc->ageFrames++;
        if (vc->releaseLeft == 0 && !vc->isSfxLoop && vc->smpIdx >= 0 && vc->smpIdx < AFN_SOUND_SAMPLE_COUNT) {
            // Looping SFX (enemy charge) hold full volume — never apply the decay
            // envelope that fades one-shots out over time.
            int decayPct = afn_pcm_decay_pct[vc->smpIdx];
            int decayMin = afn_pcm_decay_min_ms[vc->smpIdx];
            int decayMinF = (decayMin * 60 + 999) / 1000;
            if (decayPct > 0 && vc->ageFrames > decayMinF) {
                int span = vc->ageFrames - decayMinF;
                int drop = (vc->volPeak * decayPct * span) / (100 * 60);
                int target = vc->volPeak - drop;
                if (target < 0) target = 0;
                vc->vol = target;
            }
        }
        if (vc->releaseLeft == 0 && vc->noteOffTick > 0 && nowTick >= vc->noteOffTick) {
            int rms = (vc->smpIdx >= 0 && vc->smpIdx < AFN_SOUND_SAMPLE_COUNT)
                      ? afn_pcm_release_ms[vc->smpIdx] : 0;
            int rframes = (rms * 60 + 999) / 1000;
            if (!softFade) rframes = 0;
            if (rframes <= 0) { vc->playing = 0; continue; }
            vc->releaseFrames = rframes;
            vc->releaseLeft   = rframes;
        }
        if (vc->releaseLeft > 0) {
            vc->releaseLeft--;
            int target = (vc->vol * vc->releaseLeft) / vc->releaseFrames;
            if (target < 0) target = 0;
            vc->vol = target;
            if (vc->releaseLeft == 0) vc->playing = 0;
        }
    }

    if (snd_seq_active < 0 || snd_seq_active >= AFN_SOUND_INSTANCE_COUNT) { unlock(); return; }
    const AfnSndNote* notes = afn_snd_note_ptrs[snd_seq_active];
    int count = afn_snd_note_counts[snd_seq_active];
    int tpf = afn_snd_tpf[snd_seq_active];
    if (!notes || count == 0) { snd_seq_active = -1; unlock(); return; }
    snd_seq_tick += tpf;
    while (snd_seq_next < count && (notes[snd_seq_next].tick << 8) <= snd_seq_tick) {
        const AfnSndNote* n = &notes[snd_seq_next];
        if (n->smpIdx == 255) {
            int bv = (n->note << 7) | n->vel;
            snd_pitch_bend[n->channel] = bv - 8192;
            for (int i = 0; i < SND_MAX_VOICES; i++) {
                SndVoice* vc = &snd_voices[i];
                if (!vc->playing || vc->channel != n->channel) continue;
                int adj = 256 + (snd_pitch_bend[n->channel] * 256) / 49152;
                if (adj < 128) adj = 128;
                int hz = (vc->baseHz * adj) >> 8;
                hz = note_to_hz(hz, vc->note, 60);
                vc->phaseInc = hz_to_inc(hz);
            }
        } else {
            int bgmVel = (n->vel * AFN_MIDI_MASTER_VOL_FIX) >> 8;
            if (bgmVel > 127) bgmVel = 127;
            trigger_sample(n->smpIdx, n->note, bgmVel, n->dur, n->channel);
        }
        snd_seq_next++;
    }
    if (afn_snd_loop[snd_seq_active]) {
        int loopEnd = afn_snd_loop_end[snd_seq_active];
        if (loopEnd > 0 && nowTick >= loopEnd) {
            int loopStart = afn_snd_loop_start[snd_seq_active];
            snd_seq_tick = loopStart << 8;
            snd_seq_next = 0;
            while (snd_seq_next < count && notes[snd_seq_next].tick < loopStart) snd_seq_next++;
            for (int i = 0; i < 16; i++) snd_pitch_bend[i] = 0;
            for (int i = 0; i < SND_MAX_VOICES; i++) {
                SndVoice* vc = &snd_voices[i];
                // Looping SFX (enemy charge, struggle) are NOT part of the BGM
                // sequence — don't release them when the music loops.
                if (!vc->playing || vc->releaseLeft > 0 || vc->isSfxLoop) continue;
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
        int anyActive = 0;
        for (int i = 0; i < SND_MAX_VOICES; i++) if (snd_voices[i].playing) { anyActive = 1; break; }
        if (!anyActive) snd_seq_active = -1;
    }
    unlock();
}

static void mix_buffer(short* out, int frames) {
    memset(s_acc, 0, sizeof(int) * frames * 2);
    for (int v = 0; v < SND_MAX_VOICES; v++) {
        SndVoice* vc = &snd_voices[v];
        if (!vc->playing || !vc->data || vc->len <= 0) continue;
        const signed char* p8 = (const signed char*)vc->data;
        const short*       p16 = (const short*)vc->data;
        int pos = vc->pos; unsigned int frac = vc->frac, inc = vc->phaseInc;
        int len = vc->len, vol = vc->vol, is16 = vc->is16;
        int loop = vc->loop, ls = vc->loopStart;
        int loopLen = len - ls;
        for (int f = 0; f < frames; f++) {
            if (pos >= len) {
                if (loop && loopLen > 0) pos = ls + (pos - ls) % loopLen;
                else { vc->playing = 0; break; }
            }
            int i1 = pos + 1;
            if (i1 >= len) i1 = (loop && loopLen > 0) ? ls : pos;
            int s0 = is16 ? p16[pos] : (p8[pos] << 8);
            int s1 = is16 ? p16[i1]  : (p8[i1]  << 8);
            int s  = s0 + (((s1 - s0) * (int)frac) >> 16);
            s = (s * vol) >> 7;
            s_acc[f*2]   += s;
            s_acc[f*2+1] += s;
            frac += inc;
            pos  += frac >> 16;
            frac &= 0xFFFF;
        }
        vc->pos = pos; vc->frac = frac;
    }
    for (int i = 0; i < frames * 2; i++) {
        int s = s_acc[i];
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        out[i] = (short)s;
    }
}

static int audio_thread(SceSize args, void* argp) {
    (void)args; (void)argp;
    int cur = 0;
    while (s_running) {
        short* buf = s_outbuf[cur];
        lock();
        mix_buffer(buf, AUDIO_FRAMES);
        unlock();
        sceAudioOutOutput(s_port, buf);   // blocks until a buffer slot frees
        cur ^= 1;
    }
    return 0;
}

void afn_audio_init(void) {
    for (int i = 0; i < SND_MAX_VOICES; i++) snd_voices[i].playing = 0;
    s_mutex = sceKernelCreateSema("afn_snd", 0, 1, 1, 0);
    s_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_FRAMES, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (s_port < 0) return;   // no output port — stay silent
    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(s_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    s_running = 1;
    SceUID th = sceKernelCreateThread("afn_audio", audio_thread, 0x10000100, 0x4000, 0, 0, 0);
    if (th >= 0) sceKernelStartThread(th, 0, 0);
}

#endif // AFN_HAS_SOUND
