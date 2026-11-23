#ifndef VITADROP_MUSIC_H
#define VITADROP_MUSIC_H

#include <stdint.h>

#define MUSIC_SAMPLE_RATE 48000

/* Prepares the synthesizer. Call once before rendering. */
void music_init(void);

/* Fills out with `frames` stereo samples (left, right, left, right...). */
void music_render(int16_t *out, int frames);

/* Turns the music on or off. */
void music_set_enabled(int on);
int music_is_enabled(void);

#ifdef __vita__
/* Starts and stops the background music thread on the Vita. */
int music_start(void);
void music_stop(void);
#endif

#endif
