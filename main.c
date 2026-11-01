// MechaPlayer PSP - reproduce video.mpg (MPEG-1 + MP2, 44100 Hz) a pantalla completa con audio.
// CODIGO SIN PROBAR. Para pspdev (PSPSDK) + pl_mpeg.h
//
// video.mpg tiene que estar en la misma carpeta que el EBOOT.PBP.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspaudio.h>
#include <psppower.h>

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

PSP_MODULE_INFO("MechaPlayer", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

// ---------------- Configuracion ----------------
#define LOOP_VIDEO    0       // 0 = al terminar vuelve al menu, 1 = se repite siempre
#define AUDIO_GRAIN   2048    // muestras por bloque de audio (multiplo de 64)
#define SCREEN_W      480
#define SCREEN_H      272
#define BUF_W         512     // ancho real del framebuffer de la PSP

// ---------------- Estado ----------------
static uint8_t *g_vram = (uint8_t *)0x44000000;   // VRAM sin cache
static int g_vw = 0, g_vh = 0, g_x0 = 0, g_y0 = 0;

// Cola circular de audio. Un hilo escribe (wPos) y el otro lee (rPos).
#define RING_FRAMES 16384   // potencia de 2
static int16_t g_ring[RING_FRAMES * 2];
static volatile unsigned int g_wPos = 0, g_rPos = 0;
static volatile int g_audioRun = 1;

// ---------------- Boton HOME ----------------
static int exit_callback(int a1, int a2, void *c) {
    (void)a1; (void)a2; (void)c;
    sceKernelExitGame();
    return 0;
}
static int callback_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int cb = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}
static void setup_callbacks(void) {
    int th = sceKernelCreateThread("update_thread", callback_thread, 0x11, 0xFA0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}

// ---------------- Callbacks de pl_mpeg ----------------
static void on_video(plm_t *plm, plm_frame_t *frame, void *user) {
    (void)plm; (void)user;
    // Convierte directo a la VRAM (formato 8888 de la PSP = bytes R,G,B,A)
    uint8_t *dest = g_vram + ((size_t)g_y0 * BUF_W + g_x0) * 4;
    plm_frame_to_rgba(frame, dest, BUF_W * 4);
}

static void on_audio(plm_t *plm, plm_samples_t *samples, void *user) {
    (void)plm; (void)user;
    for (unsigned int i = 0; i < samples->count; i++) {
        if ((g_wPos - g_rPos) >= RING_FRAMES) return;   // cola llena
        unsigned int idx = g_wPos & (RING_FRAMES - 1);
        for (int c = 0; c < 2; c++) {
            float v = samples->interleaved[i * 2 + c];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            g_ring[idx * 2 + c] = (int16_t)(v * 32767.0f);
        }
        g_wPos++;
    }
}

// ---------------- Hilo de audio ----------------
static int16_t g_out[AUDIO_GRAIN * 2] __attribute__((aligned(64)));

static int audio_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, AUDIO_GRAIN, PSP_AUDIO_FORMAT_STEREO);
    if (ch < 0) { printf("Error: sceAudioChReserve\n"); return 0; }

    while (g_audioRun) {
        for (int i = 0; i < AUDIO_GRAIN; i++) {
            if (g_wPos != g_rPos) {
                unsigned int idx = g_rPos & (RING_FRAMES - 1);
                g_out[i * 2]     = g_ring[idx * 2];
                g_out[i * 2 + 1] = g_ring[idx * 2 + 1];
                g_rPos++;
            } else {
                g_out[i * 2] = 0;
                g_out[i * 2 + 1] = 0;
            }
        }
        sceAudioOutputBlocking(ch, PSP_AUDIO_VOLUME_MAX, g_out);
    }
    sceAudioChRelease(ch);
    return 0;
}

// ---------------- Abrir el video (busca junto al EBOOT) ----------------
static plm_t *open_video(int argc, char **argv) {
    char path[256];
    plm_t *plm = NULL;

    if (argc > 0 && argv[0]) {
        strncpy(path, argv[0], sizeof(path) - 16);
        path[sizeof(path) - 16] = 0;
        char *slash = strrchr(path, '/');
        if (slash) {
            strcpy(slash + 1, "video.mpg");
            plm = plm_create_with_filename(path);
            if (plm) return plm;
        }
    }
    plm = plm_create_with_filename("ms0:/PSP/GAME/MechaPlayer/video.mpg");
    if (plm) return plm;
    return plm_create_with_filename("video.mpg");
}

// ---------------- main ----------------
int main(int argc, char **argv) {
    setup_callbacks();
    scePowerSetClockFrequency(333, 333, 166);   // maxima velocidad de CPU

    sceDisplaySetMode(0, SCREEN_W, SCREEN_H);
    sceDisplaySetFrameBuf((void *)0x04000000, BUF_W, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
    memset(g_vram, 0, (size_t)BUF_W * SCREEN_H * 4);

    plm_t *plm = open_video(argc, argv);
    if (!plm) {
        printf("Error: no se encontro video.mpg\n");
        sceKernelDelayThread(3000000);
        sceKernelExitGame();
        return 0;
    }

    g_vw = plm_get_width(plm);
    g_vh = plm_get_height(plm);
    if (g_vw > SCREEN_W) g_vw = SCREEN_W;
    if (g_vh > SCREEN_H) g_vh = SCREEN_H;
    g_x0 = (SCREEN_W - g_vw) / 2;
    g_y0 = (SCREEN_H - g_vh) / 2;

    plm_set_loop(plm, LOOP_VIDEO);
    plm_set_audio_enabled(plm, 1);
    plm_set_audio_stream(plm, 0);
    plm_set_audio_lead_time(plm, 0.15);     // el audio se decodifica 150 ms adelantado
    plm_set_video_decode_callback(plm, on_video, NULL);
    plm_set_audio_decode_callback(plm, on_audio, NULL);

    plm_decode(plm, 0.0001);                // precarga el audio

    int th = sceKernelCreateThread("audio_thread", audio_thread, 0x12, 0x10000, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);

    // El video avanza segun el reloj del sistema
    uint64_t last = sceKernelGetSystemTimeWide();
    while (!plm_has_ended(plm)) {
        uint64_t now = sceKernelGetSystemTimeWide();
        double dt = (double)(now - last) / 1000000.0;
        last = now;
        if (dt > 0.1) dt = 0.1;
        plm_decode(plm, dt);
        sceKernelDelayThread(1000);
    }

    // esperar a que termine de sonar lo que quede
    while (g_wPos != g_rPos) sceKernelDelayThread(10000);
    sceKernelDelayThread(200000);
    g_audioRun = 0;
    sceKernelDelayThread(200000);

    plm_destroy(plm);
    sceKernelExitGame();
    return 0;
}
