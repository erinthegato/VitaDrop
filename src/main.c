/*
 * VitaDrop - send files from a phone to the PS Vita.
 *
 * 1. Connects to Wi-Fi and finds the Vita's IP address.
 * 2. Starts a small web server (see server.c).
 * 3. Shows a QR code with the address, e.g. http://192.168.1.25:8080/
 * 4. The phone scans it, opens the page in its browser, and uploads files.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <vita2d.h>

#include "qrcodegen.h"
#include "server.h"

#define PORT       8080
#define SAVE_DIR   "ux0:data/VitaDrop"
#define NET_MEMORY (1 * 1024 * 1024)

#define WHITE  RGBA8(255, 255, 255, 255)
#define BLACK  RGBA8(0, 0, 0, 255)
#define TEXT   RGBA8(232, 236, 248, 255)
#define DIM    RGBA8(154, 166, 200, 255)
#define BLUE   RGBA8(42, 99, 255, 255)
#define GREEN  RGBA8(34, 197, 94, 255)
#define BG     RGBA8(11, 16, 32, 255)

static void start_network(void)
{
    static char net_memory[NET_MEMORY];
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);

    SceNetInitParam param;
    param.memory = net_memory;
    param.size = sizeof(net_memory);
    param.flags = 0;
    sceNetInit(&param);
    sceNetCtlInit();
}

/* Fills ip with the Vita's address. Returns 1 if connected to Wi-Fi. */
static int get_ip(char *ip, size_t size)
{
    int state = 0;
    if (sceNetCtlInetGetState(&state) < 0 || state != SCE_NETCTL_STATE_CONNECTED)
        return 0;

    SceNetCtlInfo info;
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0)
        return 0;
    snprintf(ip, size, "%s", info.ip_address);
    return 1;
}

/* Draws the QR code as black and white squares. */
static void draw_qr(const uint8_t *qr, int x, int y, int area)
{
    int modules = qrcodegen_getSize(qr);
    int border = 4;                                   /* white margin scanners need */
    int cell = area / (modules + border * 2);
    if (cell < 1) cell = 1;
    int full = cell * (modules + border * 2);

    vita2d_draw_rectangle(x, y, full, full, WHITE);
    for (int row = 0; row < modules; row++)
        for (int col = 0; col < modules; col++)
            if (qrcodegen_getModule(qr, col, row))
                vita2d_draw_rectangle(x + (col + border) * cell,
                                      y + (row + border) * cell,
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

int main(void)
{
    vita2d_init();
    vita2d_set_clear_color(BG);
    vita2d_pgf *font = vita2d_load_default_pgf();

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    start_network();

    char ip[32] = "";
    char url[64] = "";
    int have_qr = 0;
    int server_started = 0;
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    static uint8_t qr_temp[qrcodegen_BUFFER_LEN_MAX];

    SceCtrlData pad;
    unsigned int previous_buttons = 0;
    int screen_off = 0;
    int frame = 0;

    while (1) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;

        /*
         * Stop the Vita from going to sleep on its own while the app is open.
         * (The screen can still turn off; transfers keep going.)
         */
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);

        if (screen_off) {
            /* Screen-off mode: any button turns the screen back on. */
            if (pressed) {
                scePowerRequestDisplayOn();
                screen_off = 0;
            } else {
                sceKernelDelayThread(100 * 1000);   /* draw nothing, save battery */
                continue;
            }
        } else {
            if (pressed & SCE_CTRL_START) break;
            if ((pressed & SCE_CTRL_TRIANGLE) && ip[0]) {
                scePowerRequestDisplayOff();
                screen_off = 1;
                continue;
            }
        }

        /* Check Wi-Fi about twice a second (it can drop, or change after sleep). */
        if (frame++ % 30 == 0) {
            char new_ip[32];
            if (get_ip(new_ip, sizeof(new_ip))) {
                if (strcmp(new_ip, ip) != 0) {
                    snprintf(ip, sizeof(ip), "%s", new_ip);
                    snprintf(url, sizeof(url), "http://%s:%d/", ip, PORT);
                    have_qr = qrcodegen_encodeText(url, qr_temp, qr, qrcodegen_Ecc_MEDIUM,
                                                   qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                                   qrcodegen_Mask_AUTO, true);
                }
                if (!server_started) {
                    server_start(SAVE_DIR, PORT);
                    server_started = 1;
                }
            } else {
                ip[0] = '\0';   /* show the "Waiting for Wi-Fi" screen */
            }
        }

        ServerStatus st;
        server_get_status(&st);

        vita2d_start_drawing();
        vita2d_clear_screen();

        if (!ip[0]) {
            vita2d_pgf_draw_text(font, 60, 240, TEXT, 1.6f, "Waiting for Wi-Fi...");
            vita2d_pgf_draw_text(font, 60, 290, DIM, 1.0f,
                                 "Connect the Vita to the same Wi-Fi network as your iPhone.");
            vita2d_pgf_draw_text(font, 60, 320, DIM, 1.0f,
                                 "(Settings > Network > Wi-Fi Settings)");
        } else {
            if (have_qr) draw_qr(qr, 40, 50, 440);

            int tx = 510;
            vita2d_pgf_draw_text(font, tx, 80, TEXT, 1.7f, "VitaDrop");
            vita2d_pgf_draw_text(font, tx, 125, DIM, 1.0f, "Scan with your iPhone camera,");
            vita2d_pgf_draw_text(font, tx, 150, DIM, 1.0f, "then tap the link to open it.");
            vita2d_pgf_draw_text(font, tx, 195, BLUE, 1.0f, url);

            char line[200], size_text[32];
            snprintf(line, sizeof(line), "Files received: %d", st.files_received);
            vita2d_pgf_draw_text(font, tx, 250, TEXT, 1.0f, line);
            format_size(st.bytes_received, size_text, sizeof(size_text));
            snprintf(line, sizeof(line), "Total: %s", size_text);
            vita2d_pgf_draw_text(font, tx, 278, TEXT, 1.0f, line);

            if (st.busy && st.current_total > 0) {
                int percent = (int)(st.current_done * 100 / st.current_total);
                vita2d_pgf_draw_textf(font, tx, 325, TEXT, 0.9f, "%.34s", st.current_name);
                vita2d_draw_rectangle(tx, 340, 400, 12, RGBA8(38, 48, 79, 255));
                vita2d_draw_rectangle(tx, 340, 4 * percent, 12, BLUE);
                vita2d_pgf_draw_textf(font, tx, 380, DIM, 0.9f, "%d%%", percent);
            } else {
                vita2d_pgf_draw_textf(font, tx, 325,
                                      st.files_received ? GREEN : DIM, 0.9f,
                                      "%.40s", st.message);
            }

            vita2d_pgf_draw_text(font, tx, 430, DIM, 0.85f, "Saved to: " SAVE_DIR "/");
            vita2d_pgf_draw_text(font, tx, 460, DIM, 0.85f, "TRIANGLE: screen off (keeps sending)");
            vita2d_pgf_draw_text(font, tx, 485, DIM, 0.85f, "START: exit");
        }

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    server_stop();
    vita2d_fini();
    vita2d_free_pgf(font);
    sceNetCtlTerm();
    sceNetTerm();
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    sceKernelExitProcess(0);
    return 0;
}
