/*
 * VitaDrop - send files from a phone to the PS Vita.
 *
 * 1. Connects to Wi-Fi and finds the Vita's IP address.
 * 2. Starts a small web server (see server.c).
 * 3. Shows a QR code with the address, e.g. http://192.168.1.25:8080/
 * 4. The phone scans it, opens the page in its browser, and uploads files.
 *
 * Buttons: CIRCLE music on/off, TRIANGLE screen off, START exit.
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
#include "music.h"
#include "server.h"
#include "ui.h"

#define PORT       8080
#define SAVE_DIR   "ux0:data/VitaDrop"
#define NET_MEMORY (1 * 1024 * 1024)

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

int main(void)
{
    vita2d_init();
    vita2d_set_clear_color(UI_CLEAR_COLOR);
    vita2d_pgf *font = vita2d_load_default_pgf();

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    start_network();
    music_start();

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
            if (pressed & SCE_CTRL_CIRCLE) music_set_enabled(!music_is_enabled());
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

        if (!ip[0])
            ui_draw_waiting(font, music_is_enabled());
        else
            ui_draw_main(font, url, qr, have_qr, &st, SAVE_DIR, music_is_enabled());

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    server_stop();
    music_stop();
    vita2d_fini();
    vita2d_free_pgf(font);
    sceNetCtlTerm();
    sceNetTerm();
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    sceKernelExitProcess(0);
    return 0;
}
