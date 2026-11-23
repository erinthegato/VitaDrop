/*
 * VitaDrop background music.
 *
 * An original, relaxed bossa-nova loop played by a tiny built-in synthesizer:
 * electric piano chords, a marimba melody, bass, shaker and rim clicks.
 * Nothing is loaded from disk; the song is the note tables below.
 *
 * music_render() is plain C, so the same code can be run on a computer to
 * record a preview (see tools/ in the repo).
 */
#include "music.h"

#include <math.h>
#include <string.h>

#define SR             MUSIC_SAMPLE_RATE
#define BPM            112.0f
#define STEPS_PER_BAR  16                  /* 16th notes */
#define BARS           16
#define SONG_STEPS     (BARS * STEPS_PER_BAR)
#define MAX_VOICES     40
#define TABLE_SIZE     4096
#define MASTER_VOLUME  0.75f

enum { PIANO, LEAD, BASS, SHAKER, RIM };

/* ---------- the song ---------- */

typedef struct { int root, fifth, notes[3]; } Chord;   /* MIDI note numbers */

enum { FMAJ7, EM7, A7, DM7, G7, CMAJ7, AM7, FM6 };

static const Chord CHORDS[] = {
    [FMAJ7] = { 41, 48, { 57, 60, 64 } },
    [EM7]   = { 40, 47, { 55, 59, 62 } },
    [A7]    = { 45, 40, { 55, 61, 64 } },
    [DM7]   = { 38, 45, { 53, 57, 60 } },
    [G7]    = { 43, 38, { 53, 59, 62 } },
    [CMAJ7] = { 36, 43, { 52, 55, 59 } },
    [AM7]   = { 45, 40, { 55, 60, 64 } },
    [FM6]   = { 41, 48, { 56, 60, 62 } },
};

/* Two chords per bar (first half, second half). */
static const unsigned char PROGRESSION[BARS][2] = {
    { FMAJ7, FMAJ7 }, { EM7, A7 },   { DM7, DM7 },   { G7, G7 },
    { CMAJ7, CMAJ7 }, { AM7, AM7 },  { DM7, G7 },    { CMAJ7, CMAJ7 },
    { FMAJ7, FMAJ7 }, { FM6, FM6 },  { EM7, EM7 },   { A7, A7 },
    { DM7, DM7 },     { G7, G7 },    { CMAJ7, A7 },  { DM7, G7 },
};

/* Melody: bar, step in bar (0-15), length in steps, MIDI note. */
typedef struct { unsigned char bar, step, len, note; } Note;

static const Note MELODY[] = {
    { 0, 0, 4, 81 }, { 0, 4, 2, 79 }, { 0, 6, 4, 76 }, { 0, 10, 2, 72 }, { 0, 12, 4, 76 },
    { 1, 0, 4, 79 }, { 1, 4, 2, 76 }, { 1, 6, 2, 71 }, { 1, 8, 4, 73 }, { 1, 12, 4, 76 },
    { 2, 0, 6, 77 }, { 2, 6, 2, 76 }, { 2, 8, 2, 74 }, { 2, 10, 2, 77 }, { 2, 12, 4, 81 },
    { 3, 0, 4, 79 }, { 3, 4, 4, 77 }, { 3, 8, 2, 74 }, { 3, 10, 2, 71 }, { 3, 12, 4, 74 },
    { 4, 0, 6, 76 }, { 4, 6, 2, 79 }, { 4, 8, 4, 83 }, { 4, 12, 4, 79 },
    { 5, 0, 4, 81 }, { 5, 4, 2, 79 }, { 5, 6, 2, 76 }, { 5, 8, 8, 72 },
    { 6, 0, 4, 74 }, { 6, 4, 2, 77 }, { 6, 6, 2, 81 }, { 6, 8, 4, 79 }, { 6, 12, 4, 77 },
    { 7, 0, 8, 76 }, { 7, 8, 2, 74 }, { 7, 10, 2, 76 }, { 7, 12, 4, 79 },
    { 8, 0, 4, 84 }, { 8, 4, 2, 81 }, { 8, 6, 4, 79 }, { 8, 10, 2, 76 }, { 8, 12, 4, 81 },
    { 9, 0, 6, 80 }, { 9, 6, 2, 79 }, { 9, 8, 4, 77 }, { 9, 12, 4, 74 },
    { 10, 0, 4, 76 }, { 10, 4, 2, 79 }, { 10, 6, 2, 83 }, { 10, 8, 8, 79 },
    { 11, 0, 4, 81 }, { 11, 4, 2, 79 }, { 11, 6, 2, 76 }, { 11, 8, 4, 73 }, { 11, 12, 4, 76 },
    { 12, 0, 6, 77 }, { 12, 6, 2, 81 }, { 12, 8, 4, 84 }, { 12, 12, 4, 81 },
    { 13, 0, 4, 83 }, { 13, 4, 2, 81 }, { 13, 6, 2, 79 }, { 13, 8, 4, 77 }, { 13, 12, 4, 74 },
    { 14, 0, 6, 76 }, { 14, 6, 2, 79 }, { 14, 8, 4, 79 }, { 14, 12, 4, 76 },
    { 15, 0, 4, 74 }, { 15, 4, 4, 77 }, { 15, 8, 4, 71 }, { 15, 12, 4, 67 },
};
#define MELODY_COUNT ((int)(sizeof(MELODY) / sizeof(MELODY[0])))

/* Bossa-nova piano rhythm: which 16th notes of the bar get a chord. */
static const unsigned char COMP_STEPS[] = { 0, 3, 6, 10, 13 };

/* ---------- synthesizer ---------- */

typedef struct {
    int active, inst, released;
    float freq, phase;
    float amp, decay, release;      /* main volume envelope */
    float bright, bright_decay;     /* how "plucky" the start of the note is */
    float attack;                   /* 0 -> 1 fade-in to avoid clicks */
    float left, right;              /* stereo position */
    int hold;                       /* samples until the key is let go */
    float last_noise;
} Voice;

static float sine_table[TABLE_SIZE + 1];
static Voice voices[MAX_VOICES];
static float samples_per_step;
static float step_clock;
static int song_step;
static unsigned int noise_seed = 22222;
static volatile int enabled = 1;

/* Echo (ping-pong delay) for a little space. */
#define DELAY_MAX 24000
static float delay_l[DELAY_MAX], delay_r[DELAY_MAX];
static int delay_len, delay_pos;

static float midi_to_freq(int note) { return 440.0f * powf(2.0f, (note - 69) / 12.0f); }

static float table_sin(float phase)  /* phase in cycles */
{
    phase -= (float)(int)phase;
    if (phase < 0) phase += 1.0f;
    float idx = phase * TABLE_SIZE;
    int i = (int)idx;
    float frac = idx - (float)i;
    return sine_table[i] + (sine_table[i + 1] - sine_table[i]) * frac;
}

static float noise(void)
{
    noise_seed = noise_seed * 1664525u + 1013904223u;
    return (float)(noise_seed >> 8) / 8388608.0f - 1.0f;
}

static float per_sample(float rate) { return expf(-rate / SR); }

static void play(int inst, int note, float volume, int length_steps, float pan)
{
    Voice *v = &voices[0];
    for (int i = 0; i < MAX_VOICES; i++) {          /* free voice, else quietest */
        if (!voices[i].active) { v = &voices[i]; break; }
        if (voices[i].amp < v->amp) v = &voices[i];
    }
    memset(v, 0, sizeof(*v));
    v->active = 1;
    v->inst = inst;
    v->freq = note > 0 ? midi_to_freq(note) : 0;
    v->amp = volume;
    v->hold = (int)(length_steps * samples_per_step);
    v->left = 1.0f - pan;
    v->right = 1.0f + pan;
    v->bright = 1.0f;

    switch (inst) {
    case PIANO:  v->decay = per_sample(2.2f); v->bright_decay = per_sample(9);  v->release = per_sample(20); break;
    case LEAD:   v->decay = per_sample(3.0f); v->bright_decay = per_sample(18); v->release = per_sample(6);  break;
    case BASS:   v->decay = per_sample(1.4f); v->bright_decay = per_sample(12); v->release = per_sample(30); break;
    case SHAKER: v->decay = per_sample(70);   v->release = 1.0f; v->hold = SR; break;
    case RIM:    v->decay = per_sample(55);   v->release = 1.0f; v->hold = SR; v->freq = 1750; break;
    }
}

/* Starts all the notes that begin on the current 16th note. */
static void play_step(int s)
{
    int bar = s / STEPS_PER_BAR, step = s % STEPS_PER_BAR;
    const Chord *c = &CHORDS[PROGRESSION[bar][step < 8 ? 0 : 1]];

    /* bass: root on the beat, fifth just before the next chord */
    if (step % 8 == 0) play(BASS, c->root, 0.30f, 5, 0.0f);
    if (step % 8 == 6) play(BASS, c->fifth, 0.22f, 2, 0.0f);

    /* piano chords in a bossa rhythm */
    for (unsigned i = 0; i < sizeof(COMP_STEPS); i++)
        if (COMP_STEPS[i] == step)
            for (int n = 0; n < 3; n++)
                play(PIANO, c->notes[n], step == 0 ? 0.085f : 0.065f, 2, -0.25f);

    /* melody */
    for (int i = 0; i < MELODY_COUNT; i++)
        if (MELODY[i].bar == bar && MELODY[i].step == step)
            play(LEAD, MELODY[i].note, 0.20f, MELODY[i].len, 0.2f);

    /* shaker on every 16th, accented on the off-beats */
    play(SHAKER, 0, step % 4 == 2 ? 0.07f : (step % 2 ? 0.025f : 0.04f), 1, 0.45f);

    /* rim clicks, alternating two patterns */
    static const unsigned char rim_a[] = { 0, 3, 6, 10, 13 }, rim_b[] = { 2, 6, 10 };
    const unsigned char *rim = bar % 2 ? rim_b : rim_a;
    int rim_count = bar % 2 ? 3 : 5;
    for (int i = 0; i < rim_count; i++)
        if (rim[i] == step) play(RIM, 0, 0.06f, 1, -0.45f);
}

static float voice_sample(Voice *v)
{
    float out = 0;
    switch (v->inst) {
    case PIANO:   /* FM electric piano */
        out = table_sin(v->phase + 0.30f * v->bright * table_sin(v->phase));
        break;
    case LEAD:    /* marimba: sine plus a quick high overtone */
        out = table_sin(v->phase) + 0.35f * v->bright * table_sin(v->phase * 3.99f);
        break;
    case BASS:    /* round bass with a little bite at the start */
        out = table_sin(v->phase) + 0.3f * v->bright * table_sin(v->phase * 2.0f);
        break;
    case SHAKER: { /* high-passed noise */
        float n = noise();
        out = n - v->last_noise;
        v->last_noise = n;
        break;
    }
    case RIM:
        out = 0.7f * table_sin(v->phase) + 0.5f * noise();
        break;
    }

    if (v->attack < 1.0f) { v->attack += 1.0f / 96; out *= v->attack; }
    out *= v->amp;

    v->phase += v->freq / SR;
    if (v->phase > 1024.0f) v->phase -= 1024.0f;
    v->amp *= v->decay;
    v->bright *= v->bright_decay;
    if (v->hold > 0 && --v->hold == 0) v->released = 1;
    if (v->released) v->amp *= v->release;
    if (v->amp < 0.0003f) v->active = 0;
    return out;
}

static float soft_clip(float x)
{
    if (x > 1.0f) return 1.0f;
    if (x < -1.0f) return -1.0f;
    return x * (1.5f - 0.5f * x * x);
}

void music_init(void)
{
    for (int i = 0; i <= TABLE_SIZE; i++)
        sine_table[i] = sinf(2.0f * 3.14159265f * (float)i / TABLE_SIZE);
    memset(voices, 0, sizeof(voices));
    memset(delay_l, 0, sizeof(delay_l));
    memset(delay_r, 0, sizeof(delay_r));
    samples_per_step = SR * 60.0f / BPM / 4.0f;
    delay_len = (int)(samples_per_step * 3);              /* a dotted-8th echo */
    if (delay_len > DELAY_MAX) delay_len = DELAY_MAX;
    delay_pos = 0;
    step_clock = 0;
    song_step = 0;
}

void music_render(int16_t *out, int frames)
{
    if (!enabled) {
        memset(out, 0, (size_t)frames * 2 * sizeof(int16_t));
        return;
    }

    for (int f = 0; f < frames; f++) {
        if (step_clock <= 0) {
            play_step(song_step);
            song_step = (song_step + 1) % SONG_STEPS;
            step_clock += samples_per_step;
        }
        step_clock -= 1.0f;

        float l = 0, r = 0;
        for (int i = 0; i < MAX_VOICES; i++) {
            if (!voices[i].active) continue;
            float s = voice_sample(&voices[i]);
            l += s * voices[i].left;
            r += s * voices[i].right;
        }

        /* ping-pong echo */
        float echo_l = delay_l[delay_pos], echo_r = delay_r[delay_pos];
        delay_l[delay_pos] = (l + r) * 0.18f + echo_r * 0.35f;
        delay_r[delay_pos] = echo_l * 0.35f;
        delay_pos = (delay_pos + 1) % delay_len;
        l += echo_l * 0.6f;
        r += echo_r * 0.6f;

        out[f * 2]     = (int16_t)(soft_clip(l * MASTER_VOLUME) * 32000.0f);
        out[f * 2 + 1] = (int16_t)(soft_clip(r * MASTER_VOLUME) * 32000.0f);
    }
}

void music_set_enabled(int on) { enabled = on ? 1 : 0; }
int music_is_enabled(void) { return enabled; }

/* ---------- Vita audio thread ---------- */
#ifdef __vita__
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#define GRAIN 1024
static volatile int music_running = 0;
static SceUID music_thread_id = -1;
static int16_t audio_buffers[2][GRAIN * 2];

static int music_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, SR,
                                   SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) return sceKernelExitDeleteThread(0);

    int volume[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, volume);

    int which = 0;
    while (music_running) {
        music_render(audio_buffers[which], GRAIN);
        sceAudioOutOutput(port, audio_buffers[which]);    /* waits until it can play */
        which ^= 1;
    }
    sceAudioOutOutput(port, NULL);                        /* let the last buffer finish */
    sceAudioOutReleasePort(port);
    return sceKernelExitDeleteThread(0);
}

int music_start(void)
{
    music_init();
    music_running = 1;
    music_thread_id = sceKernelCreateThread("vitadrop_music", music_thread,
                                            0x10000100 - 10, 0x4000, 0, 0, NULL);
    if (music_thread_id < 0) { music_running = 0; return -1; }
    return sceKernelStartThread(music_thread_id, 0, NULL);
}

void music_stop(void)
{
    if (!music_running) return;
    music_running = 0;
    sceKernelWaitThreadEnd(music_thread_id, NULL, NULL);
}
#endif
