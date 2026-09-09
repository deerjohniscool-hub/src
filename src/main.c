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
#define FRAME_DELAY_MS 50
#define MAX_VIDEOS 10
#define BUF_SIZE 512

typedef struct {
    ti_var_t file;
    uint8_t current_idx;
    char base_name[7];
    uint8_t buffer[BUF_SIZE];
    size_t buf_pos;
    size_t buf_len;
} ChunkedReader;

static uint8_t frame_mem[120 * 90];

void setup_grayscale_palette(void) {
    uint16_t palette[16];
    for (int i = 0; i < 16; i++) {
        uint8_t level = (i * 255) / 15;
        palette[i] = gfx_RGBTo1555(level, level, level);
    }
    gfx_SetPalette(palette, 32, 0);
}

bool open_next_chunk(ChunkedReader *reader) {
    char var_name[9];
    snprintf(var_name, sizeof(var_name), "%s%u", reader->base_name, reader->current_idx);
    reader->file = ti_Open(var_name, "r");
    reader->buf_pos = 0;
    reader->buf_len = 0;
    return reader->file != 0;
}

bool read_byte(ChunkedReader *reader, uint8_t *out) {
    if (reader->buf_pos >= reader->buf_len) {
        if (!reader->file) {
            if (!open_next_chunk(reader)) return false;
        }
        reader->buf_len = ti_Read(reader->buffer, 1, BUF_SIZE, reader->file);
        reader->buf_pos = 0;
        if (reader->buf_len == 0) {
            ti_Close(reader->file);
            reader->file = 0;
            reader->current_idx++;
            if (!open_next_chunk(reader)) return false;
            reader->buf_len = ti_Read(reader->buffer, 1, BUF_SIZE, reader->file);
            if (reader->buf_len == 0) return false;
        }
    }
    *out = reader->buffer[reader->buf_pos++];
    return true;
}

bool chunk_read(void *buffer, size_t bytes_to_read, ChunkedReader *reader) {
    uint8_t *out = (uint8_t *)buffer;
    for (size_t i = 0; i < bytes_to_read; i++) {
        if (!read_byte(reader, &out[i])) return false;
    }
    return true;
}

void render_frame_scaled(const uint8_t *src, uint16_t w, uint16_t h) {
    uint8_t *vbuf = gfx_vbuffer;
    uint16_t x_off = (320 - w * SCALE_FACTOR) / 2;
    uint16_t y_off = (240 - h * SCALE_FACTOR) / 2;

    for (uint16_t y = 0; y < h; y++) {
        uint8_t *row1 = vbuf + (y_off + y * 2) * 320 + x_off;
        uint8_t *row2 = row1 + 320;
        const uint8_t *s_row = src + y * w;

        for (uint16_t x = 0; x < w; x++) {
            uint8_t col = s_row[x];
            row1[0] = col; row1[1] = col;
            row2[0] = col; row2[1] = col;
            row1 += 2;
            row2 += 2;
        }
    }
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

    setup_grayscale_palette();
    gfx_FillScreen(0);

    for (uint32_t f = 0; f < total_frames; f++) {
        if (os_GetCSC() == sk_Clear) break;

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
                    if (pixels_drawn < (uint32_t)(width * height)) {
                        frame_mem[pixels_drawn++] = color;
                    }
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
                
                if (x < width && y < height) {
                    frame_mem[y * width + x] = color;
                }
            }
        }

        render_frame_scaled(frame_mem, width, height);
        gfx_BlitBuffer();
        delay(FRAME_DELAY_MS);
    }

cleanup:
    if (reader.file) {
        ti_Close(reader.file);
    }
}

int main(void) {
    gfx_Begin();
    gfx_SetDrawBuffer();

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
        gfx_FillScreen(255);
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
        gfx_FillScreen(255);
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
