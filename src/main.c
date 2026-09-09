#include <ti/screen.h>
#include <ti/getcsc.h>
#include <fileioc.h>
#include <graphx.h>
#include <sys/timers.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#define OFFSET_X 40
#define OFFSET_Y 30
#define FRAME_WIDTH 120
#define FRAME_HEIGHT 90
#define ROW_PACKED_SIZE (FRAME_WIDTH / 2) // 60 bytes per row
#define MAX_VIDEOS 10

typedef struct {
    ti_var_t file;
    uint8_t current_idx;
    char base_name[8];
} ChunkedReader;

void set_ui_palette(void) {
    uint16_t ui_palette[2] = {
        gfx_RGBTo1555(15, 15, 25),   // 0: Dark Background
        gfx_RGBTo1555(255, 255, 255) // 1: Bright White Text
    };
    gfx_SetPalette(ui_palette, sizeof(ui_palette), 0);
}

bool open_next_chunk(ChunkedReader *reader) {
    if (reader->file) {
        ti_Close(reader->file);
        reader->file = 0;
    }
    char var_name[10];
    snprintf(var_name, sizeof(var_name), "%s%u", reader->base_name, reader->current_idx);
    reader->file = ti_Open(var_name, "r");
    return reader->file != 0;
}

bool chunk_read(void *buffer, size_t bytes_to_read, ChunkedReader *reader) {
    uint8_t *out = (uint8_t *)buffer;
    size_t bytes_left = bytes_to_read;

    while (bytes_left > 0) {
        if (!reader->file) {
            if (!open_next_chunk(reader)) {
                return false;
            }
        }

        size_t read_bytes = ti_Read(out, 1, bytes_left, reader->file);
        if (read_bytes == 0) {
            ti_Close(reader->file);
            reader->file = 0;
            reader->current_idx++;
            if (!open_next_chunk(reader)) {
                return false;
            }
            continue;
        }

        bytes_left -= read_bytes;
        out += read_bytes;
    }
    return true;
}

void show_error(const char *msg1, const char *msg2) {
    set_ui_palette();
    gfx_FillScreen(0);
    gfx_SetTextFGColor(1);
    gfx_PrintStringXY(msg1, 20, 100);
    if (msg2) gfx_PrintStringXY(msg2, 20, 120);
    gfx_PrintStringXY("Press CLEAR to return", 20, 160);
    gfx_BlitBuffer();
    while (os_GetCSC() != sk_Clear);
}

void play_video(uint8_t video_slot) {
    ti_CloseAll();
    ChunkedReader reader = {0};
    snprintf(reader.base_name, sizeof(reader.base_name), "V%uDAT", video_slot);

    if (!open_next_chunk(&reader)) {
        show_error("Error: Could not open chunk 0", "Check video file installation");
        return;
    }

    char magic[6];
    if (!chunk_read(magic, 6, &reader) || memcmp(magic, "CEVID2", 6) != 0) {
        show_error("Error: Header Mismatch", "Re-convert video file");
        goto cleanup;
    }

    uint16_t width, height;
    uint8_t target_fps, num_colors;
    uint32_t total_frames;

    if (!chunk_read(&width, sizeof(uint16_t), &reader) ||
        !chunk_read(&height, sizeof(uint16_t), &reader) ||
        !chunk_read(&target_fps, sizeof(uint8_t), &reader) ||
        !chunk_read(&num_colors, sizeof(uint8_t), &reader)) {
        show_error("Error: Corrupt header data", "Re-convert video file");
        goto cleanup;
    }

    if (width != FRAME_WIDTH || height != FRAME_HEIGHT || num_colors > 16) {
        show_error("Error: Invalid dimensions/palette", "Expected 120x90, 16 colors");
        goto cleanup;
    }

    uint16_t palette[16];
    if (!chunk_read(palette, num_colors * sizeof(uint16_t), &reader)) {
        show_error("Error: Corrupt palette data", "Re-convert video file");
        goto cleanup;
    }

    if (!chunk_read(&total_frames, sizeof(uint32_t), &reader) || total_frames == 0) {
        show_error("Error: Zero frames found", NULL);
        goto cleanup;
    }

    gfx_SetPalette(palette, num_colors * sizeof(uint16_t), 0);

    if (target_fps == 0) target_fps = 12;
    uint32_t ticks_per_frame = 32768 / target_fps;

    timer_Enable(1, TIMER_32K, TIMER_NOINT, TIMER_UP);
    gfx_FillScreen(0);

    for (uint32_t f = 0; f < total_frames; f++) {
        timer_Set(1, 0);

        bool frame_failed = false;

        for (uint8_t y = 0; y < FRAME_HEIGHT; y++) {
            uint8_t line_buf[ROW_PACKED_SIZE];
            if (!chunk_read(line_buf, ROW_PACKED_SIZE, &reader)) {
                frame_failed = true;
                break;
            }

            uint8_t *src = line_buf;
            uint8_t *line_ptr = &gfx_vbuffer[(OFFSET_Y + (y << 1)) * 320 + OFFSET_X];

            for (uint8_t x = 0; x < FRAME_WIDTH; x += 2) {
                uint8_t val = *src++;
                uint8_t c1 = val >> 4;
                uint8_t c2 = val & 0x0F;

                line_ptr[0] = c1;
                line_ptr[1] = c1;
                line_ptr[320] = c1;
                line_ptr[321] = c1;

                line_ptr[2] = c2;
                line_ptr[3] = c2;
                line_ptr[322] = c2;
                line_ptr[323] = c2;

                line_ptr += 4;
            }
        }

        if (frame_failed) break;

        gfx_BlitBuffer();

        bool exit_requested = false;
        while (timer_Get(1) < ticks_per_frame) {
            if (os_GetCSC() == sk_Clear) {
                exit_requested = true;
                break;
            }
        }

        if (exit_requested) break;
    }

    timer_Disable(1);

cleanup:
    ti_CloseAll();
}

int main(void) {
    ti_CloseAll();
    gfx_Begin();
    gfx_SetDrawBuffer();

    uint8_t found_count = 0;
    uint8_t slots[MAX_VIDEOS];

    for (uint8_t i = 0; i < MAX_VIDEOS; i++) {
        char name[10];
        snprintf(name, sizeof(name), "V%uDAT0", i);
        ti_var_t f = ti_Open(name, "r");
        if (f) {
            ti_Close(f);
            slots[found_count++] = i;
        }
    }

    if (found_count == 0) {
        show_error("No Video Files Found!", "Transfer V0DAT0 to calculator");
        gfx_End();
        return 0;
    }

    uint8_t selected_index = 0;

    while (1) {
        set_ui_palette();
        gfx_FillScreen(0);
        gfx_SetTextFGColor(1);

        gfx_PrintStringXY("TI-84 CE Color Video Player", 50, 20);
        gfx_PrintStringXY("---------------------------", 50, 32);

        char str[32];
        snprintf(str, sizeof(str), "< Video %u of %u (Slot V%uDAT) >", selected_index + 1, found_count, slots[selected_index]);
        gfx_PrintStringXY(str, 40, 100);

        gfx_PrintStringXY("Controls:", 40, 150);
        gfx_PrintStringXY("[LEFT / RIGHT] : Switch Video", 40, 170);
        gfx_PrintStringXY("[2nd]          : Play Video", 40, 185);
        gfx_PrintStringXY("[CLEAR]        : Exit App / Stop", 40, 200);

        gfx_BlitBuffer();

        uint8_t key = os_GetCSC();
        if (key == sk_Left) {
            if (selected_index > 0) selected_index--;
            else selected_index = found_count - 1;
        } else if (key == sk_Right) {
            if (selected_index < found_count - 1) selected_index++;
            else selected_index = 0;
        } else if (key == sk_2nd) {
            play_video(slots[selected_index]);
        } else if (key == sk_Clear) {
            break;
        }
    }

    gfx_End();
    ti_CloseAll();
    return 0;
}
