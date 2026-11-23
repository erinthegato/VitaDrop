/*
 * VitaDrop screen drawing.
 *
 * Theme: clean white and sky blue, soft pinstripes, rounded white cards and
 * glossy pill-shaped buttons. Change the colours below to restyle the app.
 */
#include <stdio.h>
#include <string.h>

#include "qrcodegen.h"
#include "ui.h"

#define SCREEN_W 960
#define SCREEN_H 544

/* ---------- colours ---------- */
#define WHITE    RGBA8(255, 255, 255, 255)
#define BLACK    RGBA8(30, 34, 40, 255)
#define STRIPE   RGBA8(234, 242, 247, 255)
#define BORDER   RGBA8(198, 225, 238, 255)
#define SKY      RGBA8(52, 190, 237, 255)    /* main accent */
#define SKY_DARK RGBA8(22, 146, 203, 255)    /* button outline, link text */
#define SKY_LITE RGBA8(132, 219, 247, 255)   /* button shine */
#define PALE     RGBA8(232, 245, 251, 255)   /* address box */
#define TRACK    RGBA8(220, 233, 241, 255)   /* empty progress bar */
#define TEXT     RGBA8(68, 76, 86, 255)
#define DIM      RGBA8(140, 150, 161, 255)
#define GREEN    RGBA8(70, 180, 70, 255)

/* ---------- shapes ---------- */

/* A filled rectangle with rounded corners (colours must be fully opaque). */
static void round_rect(float x, float y, float w, float h, float r, unsigned int c)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    vita2d_draw_rectangle(x + r, y, w - 2 * r, h, c);
    vita2d_draw_rectangle(x, y + r, r, h - 2 * r, c);
    vita2d_draw_rectangle(x + w - r, y + r, r, h - 2 * r, c);
    vita2d_draw_fill_circle(x + r, y + r, r, c);
    vita2d_draw_fill_circle(x + w - r, y + r, r, c);
    vita2d_draw_fill_circle(x + r, y + h - r, r, c);
    vita2d_draw_fill_circle(x + w - r, y + h - r, r, c);
}

/* A white card with a thin sky-blue outline. */
static void card(float x, float y, float w, float h)
{
    round_rect(x, y, w, h, 22, BORDER);
    round_rect(x + 3, y + 3, w - 6, h - 6, 19, WHITE);
}

/* A glossy pill-shaped button with a label. */
static void pill_button(vita2d_pgf *font, float x, float y, const char *label)
{
    float tw = vita2d_pgf_text_width(font, 0.85f, label);
    float w = tw + 44, h = 36;
    round_rect(x, y, w, h, h / 2, SKY_DARK);
    round_rect(x + 2, y + 2, w - 4, h - 4, (h - 4) / 2, SKY);
    round_rect(x + 10, y + 4, w - 20, 7, 3.5f, SKY_LITE);            /* shine */
    vita2d_pgf_draw_text(font, x + 22, y + 25, WHITE, 0.85f, label);
}

static float pill_width(vita2d_pgf *font, const char *label)
{
    return vita2d_pgf_text_width(font, 0.85f, label) + 44;
}

static void text_centered(vita2d_pgf *font, float cx, float y, unsigned int c,
                          float scale, const char *text)
{
    float w = vita2d_pgf_text_width(font, scale, text);
    vita2d_pgf_draw_text(font, cx - w / 2, y, c, scale, text);
}

/* Pinstriped background, white title bar on top, button bar at the bottom. */
static void frame(vita2d_pgf *font, int show_screen_off, int music_on)
{
    for (int y = 0; y < SCREEN_H; y += 8)
        vita2d_draw_rectangle(0, y, SCREEN_W, 3, STRIPE);

    /* title bar */
    vita2d_draw_rectangle(0, 0, SCREEN_W, 68, WHITE);
    vita2d_draw_rectangle(0, 68, SCREEN_W, 4, SKY);
    vita2d_draw_rectangle(0, 72, SCREEN_W, 2, BORDER);
    vita2d_pgf_draw_text(font, 36, 46, SKY, 1.6f, "VitaDrop");
    vita2d_pgf_draw_text(font, 36 + vita2d_pgf_text_width(font, 1.6f, "VitaDrop") + 16,
                         44, DIM, 0.9f, "File Transfer");

    /* bottom bar with buttons, right-aligned */
    vita2d_draw_rectangle(0, 490, SCREEN_W, 2, BORDER);
    vita2d_draw_rectangle(0, 492, SCREEN_W, SCREEN_H - 492, WHITE);
    float x = SCREEN_W - 30 - pill_width(font, "START  Exit");
    pill_button(font, x, 500, "START  Exit");
    if (show_screen_off) {
        x -= 14 + pill_width(font, "TRIANGLE  Screen off");
        pill_button(font, x, 500, "TRIANGLE  Screen off");
    }
    const char *music_label = music_on ? "CIRCLE  Music: On" : "CIRCLE  Music: Off";
    x -= 14 + pill_width(font, music_label);
    pill_button(font, x, 500, music_label);
}

/* Draws the QR code centred inside a square box. */
static void draw_qr(const uint8_t *qr, int box_x, int box_y, int box)
{
    int modules = qrcodegen_getSize(qr);
    int border = 4;                               /* white margin scanners need */
    int cell = box / (modules + border * 2);
    if (cell < 1) cell = 1;
    int full = cell * (modules + border * 2);
    int x = box_x + (box - full) / 2;
    int y = box_y + (box - full) / 2;

    vita2d_draw_rectangle(x, y, full, full, WHITE);
    for (int row = 0; row < modules; row++)
        for (int col = 0; col < modules; col++)
            if (qrcodegen_getModule(qr, col, row))
                vita2d_draw_rectangle(x + (col + border) * cell, y + (row + border) * cell,
                                      cell, cell, BLACK);
}

static void format_size(uint64_t bytes, char *out, size_t size)
{
    if (bytes >= 1024ULL * 1024 * 1024)
        snprintf(out, size, "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024 * 1024)
        snprintf(out, size, "%.1f MB", bytes / (1024.0 * 1024));
    else
        snprintf(out, size, "%.0f KB", bytes / 1024.0);
}

/* ---------- screens ---------- */

void ui_draw_waiting(vita2d_pgf *font, int music_on)
{
    frame(font, 0, music_on);
    card(230, 150, 500, 250);

    /* a little Wi-Fi symbol made of arcs (circles masked by white) */
    float cx = 480, cy = 255;
    vita2d_draw_fill_circle(cx, cy, 46, SKY);
    vita2d_draw_fill_circle(cx, cy, 36, WHITE);
    vita2d_draw_fill_circle(cx, cy, 26, SKY);
    vita2d_draw_fill_circle(cx, cy, 16, WHITE);
    vita2d_draw_fill_circle(cx, cy, 7, SKY);
    vita2d_draw_rectangle(cx - 50, cy, 100, 50, WHITE);           /* keep top half only */

    text_centered(font, 480, 300, TEXT, 1.3f, "Waiting for Wi-Fi...");
    text_centered(font, 480, 335, DIM, 0.9f, "Connect the Vita to the same network as your phone.");
    text_centered(font, 480, 362, DIM, 0.9f, "Settings > Network > Wi-Fi Settings");
}

void ui_draw_main(vita2d_pgf *font, const char *url, const uint8_t *qr, int have_qr,
                  const ServerStatus *st, const char *save_dir, int music_on,
                  int shortcut_ready)
{
    char line[200], size_text[32];
    frame(font, 1, music_on);

    /* left card: QR code */
    card(30, 92, 390, 384);
    if (have_qr) draw_qr(qr, 50, 104, 350);

    /* right card: instructions, address and status */
    float x = 466, w = 440;
    card(440, 92, 490, 384);

    vita2d_pgf_draw_text(font, x, 136, TEXT, 1.3f, "Scan to send files");
    vita2d_pgf_draw_text(font, x, 168, DIM, 0.9f, "Point your iPhone camera at the code,");
    vita2d_pgf_draw_text(font, x, 192, DIM, 0.9f, "or type this address in any browser:");

    round_rect(x, 206, w, 42, 21, BORDER);
    round_rect(x + 2, 208, w - 4, 38, 19, PALE);
    text_centered(font, x + w / 2, 234, SKY_DARK, 1.05f, url);

    vita2d_draw_rectangle(x, 266, w, 2, STRIPE);

    /* counters */
    vita2d_pgf_draw_text(font, x, 294, DIM, 0.8f, "FILES RECEIVED");
    snprintf(line, sizeof(line), "%d", st->files_received);
    vita2d_pgf_draw_text(font, x, 332, TEXT, 1.6f, line);

    vita2d_pgf_draw_text(font, x + 230, 294, DIM, 0.8f, "TOTAL SIZE");
    format_size(st->bytes_received, size_text, sizeof(size_text));
    vita2d_pgf_draw_text(font, x + 230, 332, TEXT, 1.6f, size_text);

    vita2d_draw_rectangle(x, 350, w, 2, STRIPE);

    /* current transfer or last message */
    if (st->busy && st->current_total > 0) {
        int percent = (int)(st->current_done * 100 / st->current_total);
        snprintf(line, sizeof(line), "%.30s", st->current_name);
        vita2d_pgf_draw_text(font, x, 382, TEXT, 0.9f, line);
        snprintf(line, sizeof(line), "%d%%", percent);
        vita2d_pgf_draw_text(font, x + w - vita2d_pgf_text_width(font, 0.9f, line), 382,
                             SKY_DARK, 0.9f, line);

        round_rect(x, 394, w, 18, 9, TRACK);
        float fill = w * percent / 100.0f;
        if (fill > 0 && fill < 18) fill = 18;
        if (fill > 0) {
            round_rect(x, 394, fill, 18, 9, SKY);
            round_rect(x + 4, 396, fill - 8 > 0 ? fill - 8 : 0, 6, 3, SKY_LITE);
        }
    } else {
        unsigned int color = st->files_received ? GREEN : DIM;
        if (st->files_received)
            vita2d_draw_fill_circle(x + 7, 390, 7, GREEN);
        snprintf(line, sizeof(line), "%.40s", st->message);
        vita2d_pgf_draw_text(font, x + (st->files_received ? 22 : 0), 397, color, 0.9f, line);
    }

    if (shortcut_ready) {
        vita2d_draw_fill_circle(x + 5, 433, 5, SKY);
        vita2d_pgf_draw_text(font, x + 16, 438, DIM, 0.8f, "Share-menu shortcut: vitadrop.local");
    }
    snprintf(line, sizeof(line), "Saved to %s/", save_dir);
    vita2d_pgf_draw_text(font, x, 460, DIM, 0.8f, line);
}
