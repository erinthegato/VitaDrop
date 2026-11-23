#ifndef VITADROP_UI_H
#define VITADROP_UI_H

#include <stdint.h>
#include <vita2d.h>
#include "server.h"

/* Background colour the screen is cleared to. */
#define UI_CLEAR_COLOR RGBA8(248, 251, 253, 255)

/* Shown while the Vita isn't connected to Wi-Fi yet. */
void ui_draw_waiting(vita2d_pgf *font, int music_on);

/* The main screen: QR code on the left, address and transfer status on the right. */
void ui_draw_main(vita2d_pgf *font, const char *url, const uint8_t *qr, int have_qr,
                  const ServerStatus *st, const char *save_dir, int music_on,
                  int shortcut_ready);

#endif
