// Affinity PSP runtime — audio (software mixer + sequencer).
// The PSP has no per-sample hardware mixer like the NDS's 16 channels, so we
// software-mix all voices into a 44100 Hz stereo buffer fed to sceAudio on a
// dedicated thread. The sequencer / envelope / voice logic mirrors the NDS
// runtime (source/audio.c); only the channel layer is replaced by the mixer.
#pragma once

void afn_audio_init(void);                       // bring up output + mixer thread (once)
void afn_audio_tick(void);                        // advance sequence + envelopes (60 Hz)
void afn_play_sound(int instanceId);              // start a sound instance (MIDI seq or SFX)
void afn_play_sfx(int smpIdx, int gain, int fifo);// fire a one-shot/looping SFX sample
void afn_stop_sound(void);                        // stop everything
void afn_stop_sfx_sample(int smpIdx);             // stop voices playing a given sample
