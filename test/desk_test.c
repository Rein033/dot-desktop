/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test: paint the desktop at a size and write it as raw RGB565 to argv[3]. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "desk.h"
static int marks;
static void mark(gfx_rect_t r) { (void)r; marks++; }
int main(int argc, char **argv)
{
    if (argc < 4) return 2;
    int w = atoi(argv[1]), h = atoi(argv[2]);
    uint16_t *fb = calloc((size_t)w * h, 2);
    if (!desk_init(w, h)) return 1;
    desk_status_t st = {0};
    st.wifi_up = 1; strcpy(st.ip, "192.168.1.57"); strcpy(st.ssid, "home"); st.rssi = -58;
    st.batt_known = 1; st.batt_pct = 76; st.vbat_mv = 3950; st.clients = 1; st.fps = 22; st.kbps = 850;
    st.hw_jpeg = 1; st.quality = 70; st.res = 0; st.uptime_s = 3725;
    st.heap_int_kb = 120; st.heap_psram_kb = 27000; st.sd_ok = 1;
    desk_status(&st); desk_clock(14, 5, 9);
    desk_act_t a; while (desk_take_action(&a)) {}
    desk_files_begin(0);
    desk_files_entry("music", 0, 1); desk_files_entry("photos", 0, 1);
    desk_files_entry("notes.txt", 1234, 0); desk_files_entry("firmware.bin", 2345678, 0);
    desk_files_end(1, 0);
    const char *t = "Hello from the Dot!\nThis desktop is streamed over Wi-Fi.";
    desk_notes_set(t, strlen(t));
    desk_pointer_abs(w / 2 + 40, h / 2 + 30);
    desk_render(fb, mark);
    if (argc > 4) { /* click the Display icon, drag the Notes title */
        desk_pointer_abs(60, 330); desk_button(1); desk_button(0);
        desk_render(fb, mark);
    }
    FILE *f = fopen(argv[3], "wb"); fwrite(fb, 2, (size_t)w * h, f); fclose(f);
    printf("marks %d\n", marks);
    return 0;
}
