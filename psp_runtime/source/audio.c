// Affinity PSP runtime — audio: software mixer + MIDI/SFX sequencer.
//
// The NDS port offloads mixing to 16 hardware channels (soundPlaySample); the
// PSP has no such per-sample mixer, so — like the GBA — we mix in software. A
// dedicated thread sums every active voice (resampled through a phase
// accumulator) into a double-buffered 44100 Hz stereo buffer and pushes it with
// sceAudioOutputBlocking. The sequencer, envelope (decay/release) and voice
// allocation logic is ported from nds_runtime/source/audio.c; each libnds call
// is replaced by a field write on a software voice that the mixer reads:
//   soundPlaySample -> set up a phase-accumulator voice
//   soundSetFreq    -> recompute phaseInc
//   soundSetVolume  -> voice gain
//   soundKill       -> deactivate the voice
// The voice table is shared between the main thread (afn_audio_tick at 60 Hz +
// afn_play_*) and the mixer thread, guarded by a semaphore.
#include "audio.h"
#include "psp_sound.h"

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspaudio.h>
#include <string.h>

#ifndef AFN_HAS_SOUND
// Project has no audio — keep the entry points defined so main.c / script link.
void afn_audio_init(void) {}
void afn_audio_tick(void) {}
void afn_play_sound(int id) { (void)id; }
void afn_play_sfx(int s, int g, int f) { (void)s; (void)g; (void)f; }
void afn_stop_sound(void) {}
void afn_stop_sfx_sample(int s) { (void)s; }
#else

// ---- Output config --------------------------------------------------------
#define OUT_RATE     44100
#define AUDIO_FRAMES 1024            // stereo frames per output buffer (mult of 64)
#define SND_MAX_VOICES 16

// ---- Software voice -------------------------------------------------------
typedef struct {
    int          playing;     // 1 = producing sound (mixer reads), 0 = free
    const void*  data;        // PCM pointer (signed char* or short*)
    int          is16;        // 1 = 16-bit samples
    int          len;         // sample count
    int          loop;        // 1 = loop
    int          loopStart;   // loop restart sample
    int          pos;         // integer sample index (full range — len can be >1M)
    unsigned int frac;        // 16.16 fractional accumulator (0..65535)
    unsigned int phaseInc;    // 16.16 fixed source samples per output sample
    int          vol;         // 0..127 (mixer gain)

    // Sequencer / envelope bookkeeping (same roles as the NDS SndVoice):
    int smpIdx, channel, note, baseHz;
    int noteOffTick;
    int volPeak, ageFrames, releaseFrames, releaseLeft, isSfxLoop;
} SndVoice;

static SndVoice snd_voices[SND_MAX_VOICES];
static int snd_voice_count = SND_MAX_VOICES;

// Sequencer state
static int snd_seq_active = -1;     // playing instance, or -1
static int snd_seq_tick   = 0;      // 8.8 fixed ticks elapsed
static int snd_seq_next   = 0;      // next note index
static int snd_pitch_bend[16];

// Output / threading
static SceUID s_mutex = -1;
static int    s_ch = -1;
static volatile int s_running = 0;
static short  s_outbuf[2][AUDIO_FRAMES * 2] __attribute__((aligned(64)));
static int    s_acc[AUDIO_FRAMES * 2];

static inline void lock(void)   { if (s_mutex >= 0) sceKernelWaitSema(s_mutex, 1, 0); }
static inline void unlock(void) { if (s_mutex >= 0) sceKernelSignalSema(s_mutex, 1); }

// ---- Note -> frequency (equal temperament, 16.16 table) -------------------
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

// ---- Voice helpers (assume the lock is held) ------------------------------
static unsigned int active_mask(void) {
    unsigned int m = 0;
    for (int i = 0; i < snd_voice_count; i++)
        if (snd_voices[i].playing) m |= (1u << i);
    return m;
}

static int alloc_voice(void) {
    for (int i = 0; i < snd_voice_count; i++)
        if (!snd_voices[i].playing) return i;
    // All busy: steal the oldest voice already in its release ramp; if none is
    // releasing, drop the new note rather than cut an active sustain.
    int best = -1, bestAge = -1;
    for (int i = 0; i < snd_voice_count; i++)
        if (snd_voices[i].releaseLeft > 0 && snd_voices[i].ageFrames > bestAge) {
            best = i; bestAge = snd_voices[i].ageFrames;
        }
    if (best < 0) return -1;
    snd_voices[best].playing = 0;
    return best;
}

static void trigger_sample(int smpIdx, int note, int vel, int durTicks, int ch) {
    if (smpIdx < 0 || smpIdx >= AFN_SOUND_SAMPLE_COUNT) return;

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
    if (v < 0) return;

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
}

static void play_sfx_locked(int smpIdx, int gain, int fifo) {
    (void)fifo;
    if (gain <= 0) gain = 127;
    // Reclaim any finished voice; if fully saturated steal the oldest one.
    unsigned int active = active_mask();
    if (active == ((1u << snd_voice_count) - 1)) {
        int best = -1, bestAge = -1;
        for (int i = 0; i < snd_voice_count; i++)
            if (snd_voices[i].releaseLeft > 0 && snd_voices[i].ageFrames > bestAge) { best = i; bestAge = snd_voices[i].ageFrames; }
        if (best < 0)
            for (int i = 0; i < snd_voice_count; i++)
                if (snd_voices[i].ageFrames > bestAge) { best = i; bestAge = snd_voices[i].ageFrames; }
        if (best >= 0) snd_voices[best].playing = 0;
    }
    trigger_sample(smpIdx, 60, gain, 0, 15);
}

// ---- Public API -----------------------------------------------------------
void afn_play_sfx(int smpIdx, int gain, int fifo) {
    lock();
    play_sfx_locked(smpIdx, gain, fifo);
    unlock();
}

void afn_play_sound(int instanceId) {
    if (instanceId < 0 || instanceId >= AFN_SOUND_INSTANCE_COUNT) return;
    lock();
    if (afn_snd_is_sfx[instanceId]) {
        // SFX-type instance: fire its one-shot sample directly.
        play_sfx_locked(afn_snd_sfx_sample[instanceId],
                        afn_snd_sfx_gain[instanceId],
                        afn_snd_sfx_fifo[instanceId]);
        unlock();
        return;
    }
    if (afn_snd_note_ptrs[instanceId] && afn_snd_note_counts[instanceId] > 0) {
        snd_seq_active = instanceId;
        snd_seq_tick   = 0;
        snd_seq_next   = 0;
        for (int i = 0; i < 16; i++) snd_pitch_bend[i] = 0;
        int editorVoices = afn_snd_voices[instanceId];
        snd_voice_count = SND_MAX_VOICES;
        if (editorVoices > 0 && editorVoices < SND_MAX_VOICES / 2)
            snd_voice_count = editorVoices;
    }
    unlock();
}

void afn_stop_sound(void) {
    lock();
    snd_seq_active = -1;
    for (int i = 0; i < SND_MAX_VOICES; i++) snd_voices[i].playing = 0;
    unlock();
}

void afn_stop_sfx_sample(int smpIdx) {
    lock();
    for (int i = 0; i < SND_MAX_VOICES; i++)
        if (snd_voices[i].playing && snd_voices[i].smpIdx == smpIdx)
            snd_voices[i].playing = 0;
    unlock();
}

// ---- Sequencer + envelopes (main thread, 60 Hz) ---------------------------
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

        // Decay during sustained hold.
        if (vc->releaseLeft == 0 && vc->smpIdx >= 0 && vc->smpIdx < AFN_SOUND_SAMPLE_COUNT) {
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

        // Start release once duration elapses.
        if (vc->releaseLeft == 0 && vc->noteOffTick > 0 && nowTick >= vc->noteOffTick) {
            int rms = (vc->smpIdx >= 0 && vc->smpIdx < AFN_SOUND_SAMPLE_COUNT)
                      ? afn_pcm_release_ms[vc->smpIdx] : 0;
            int rframes = (rms * 60 + 999) / 1000;
            if (!softFade) rframes = 0;
            if (rframes <= 0) { vc->playing = 0; continue; }
            vc->releaseFrames = rframes;
            vc->releaseLeft   = rframes;
        }

        // Tick release ramp.
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
                if (!vc->playing || vc->releaseLeft > 0) continue;
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

// ---- Mixer (audio thread) -------------------------------------------------
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
                if (loop && loopLen > 0) {
                    pos = ls + (pos - ls) % loopLen;
                } else { vc->playing = 0; break; }
            }
            int s = is16 ? p16[pos] : (p8[pos] << 8);
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
        sceKernelDcacheWritebackRange(buf, sizeof(short) * AUDIO_FRAMES * 2);
        sceAudioOutputBlocking(s_ch, PSP_AUDIO_VOLUME_MAX, buf);
        cur ^= 1;
    }
    return 0;
}

void afn_audio_init(void) {
    for (int i = 0; i < SND_MAX_VOICES; i++) snd_voices[i].playing = 0;
    s_mutex = sceKernelCreateSema("afn_snd", 0, 1, 1, 0);
    s_ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, AUDIO_FRAMES, PSP_AUDIO_FORMAT_STEREO);
    if (s_ch < 0) return;   // no output channel — stay silent
    s_running = 1;
    SceUID th = sceKernelCreateThread("afn_audio", audio_thread, 0x12, 0x4000, 0, 0);
    if (th >= 0) sceKernelStartThread(th, 0, 0);
}

#endif // AFN_HAS_SOUND
