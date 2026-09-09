#include <ti/screen.h>
#include <ti/getcsc.h>
#include <fileioc.h>
#include <graphx.h>
#include <sys/timers.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#define SCALE_FACTOR 2
#define FRAME_DELAY 33
#define MAX_VIDEOS 10

typedef struct {
    ti_var_t file;
    uint8_t current_idx;
    char base_name[7];
} ChunkedReader;

bool open_next_chunk(ChunkedReader *reader) {
    char var_name[9];
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
        bytes_left -= read_bytes;
        out += read_bytes;

        if (bytes_left > 0) {
            ti_Close(reader->file);
            reader->file = 0;
            reader->current_idx++;
        }
    }
    return true;
}

void draw_scaled_pixel(uint8_t x, uint8_t y, uint8_t color_idx) {
    gfx_SetColor(color_idx & 0x03);
    gfx_FillRectangle(x * SCALE_FACTOR, y * SCALE_FACTOR, SCALE_FACTOR, SCALE_FACTOR);
}

bool delay_and_check_exit(uint16_t ms) {
    uint16_t elapsed = 0;
    while (elapsed < ms) {
        if (os_GetCSC() == sk_Clear) {
            return true; // CLEAR pressed
        }
        delay(5);
        elapsed += 5;
    }
    return false;
}

void play_video(uint8_t video_slot) {
    ChunkedReader reader = {0};
    snprintf(reader.base_name, sizeof(reader.base_name), "V%uDAT", video_slot);

    if (!open_next_chunk(&reader)) return;

    char magic[6];
    if (!chunk_read(magic, 6, &reader)) goto cleanup;

    uint16_t width, height;
    uint32_t total_frames;
    if (!chunk_read(&width, sizeof(uint16_t), &reader)) goto cleanup;
    if (!chunk_read(&height, sizeof(uint16_t), &reader)) goto cleanup;
    if (!chunk_read(&total_frames, sizeof(uint32_t), &reader)) goto cleanup;

    gfx_FillScreen(0);

    for (uint32_t f = 0; f < total_frames; f++) {
        uint8_t frame_type;
        if (!chunk_read(&frame_type, 1, &reader)) break;

        if (frame_type == 0) { 
            uint32_t rle_len;
            if (!chunk_read(&rle_len, sizeof(uint32_t), &reader)) break;

            uint32_t pixels_drawn = 0;
            uint32_t bytes_read = 0;

            while (bytes_read < rle_len) {
                uint8_t count, color;
                if (!chunk_read(&count, 1, &reader)) break;
                if (!chunk_read(&color, 1, &reader)) break;
                bytes_read += 2;

                for (uint8_t i = 0; i < count; i++) {
                    uint8_t x = pixels_drawn % width;
                    uint8_t y = pixels_drawn / width;
                    draw_scaled_pixel(x, y, color);
                    pixels_drawn++;
                }
            }
        } else if (frame_type == 1) { 
            uint16_t num_changes;
            if (!chunk_read(&num_changes, sizeof(uint16_t), &reader)) break;

            for (uint16_t i = 0; i < num_changes; i++) {
                uint8_t x, y, color;
                if (!chunk_read(&x, 1, &reader)) break;
                if (!chunk_read(&y, 1, &reader)) break;
                if (!chunk_read(&color, 1, &reader)) break;
                draw_scaled_pixel(x, y, color);
            }
        }

        gfx_BlitBuffer();

        // Check for CLEAR press during the frame delay
        if (delay_and_check_exit(FRAME_DELAY)) {
            break;
        }
    }

cleanup:
    if (reader.file) {
        ti_Close(reader.file);
    }
}

int main(void) {
    gfx_Begin();
    gfx_SetDrawBuffer();

    uint16_t grayscale_palette[4] = {
        gfx_RGBTo1555(0, 0, 0),        // 0: Black
        gfx_RGBTo1555(85, 85, 85),    // 1: Dark Gray
        gfx_RGBTo1555(170, 170, 170), // 2: Light Gray
        gfx_RGBTo1555(255, 255, 255)  // 3: White
    };
    gfx_SetPalette(grayscale_palette, sizeof(grayscale_palette), 0);

    uint8_t found_count = 0;
    uint8_t slots[MAX_VIDEOS];

    for (uint8_t i = 0; i < MAX_VIDEOS; i++) {
        char name[9];
        snprintf(name, sizeof(name), "V%uDAT0", i);
        ti_var_t f = ti_Open(name, "r");
        if (f) {
            ti_Close(f);
            slots[found_count++] = i;
        }
    }

    if (found_count == 0) {
        gfx_FillScreen(3);
        gfx_SetTextFGColor(0);
        gfx_PrintStringXY("No Video Files Found!", 80, 110);
        gfx_PrintStringXY("Press CLEAR to exit", 85, 130);
        gfx_BlitBuffer();
        while (os_GetCSC() != sk_Clear);
        gfx_End();
        return 0;
    }

    uint8_t selected_index = 0;

    while (1) {
        gfx_FillScreen(3);
        gfx_SetTextFGColor(0);

        gfx_PrintStringXY("TI-84 CE Video Player", 80, 20);
        gfx_PrintStringXY("---------------------", 80, 32);

        char str[32];
        snprintf(str, sizeof(str), "< Video %u of %u (Slot V%uDAT) >", selected_index + 1, found_count, slots[selected_index]);
        gfx_PrintStringXY(str, 50, 100);

        gfx_PrintStringXY("Controls:", 40, 150);
        gfx_PrintStringXY("[LEFT / RIGHT] : Switch Video", 40, 170);
        gfx_PrintStringXY("[2nd]          : Play Video", 40, 185);
        gfx_PrintStringXY("[CLEAR]        : Exit App", 40, 200);

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
    return 0;
}
