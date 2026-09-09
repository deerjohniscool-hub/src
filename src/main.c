#include <ti/screen.h>
#include <fileioc.h>
#include <graphx.h>
#include <keypadc.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#define OFFSET_X 40
#define OFFSET_Y 30
#define MAX_VIDEOS 10

typedef struct {
    ti_var_t file;
    uint8_t slot;
    uint8_t chunk_idx;
    const uint8_t *ptr;
    const uint8_t *end;
} AppVarStream;

static void build_var_name(char *dest, uint8_t slot, uint8_t chunk) {
    dest[0] = 'V';
    dest[1] = '0' + (slot % 10);
    dest[2] = 'D';
    dest[3] = 'A';
    dest[4] = 'T';
    if (chunk < 10) {
        dest[5] = '0' + chunk;
        dest[6] = '\0';
    } else {
        dest[5] = '0' + (chunk / 10);
        dest[6] = '0' + (chunk % 10);
        dest[7] = '\0';
    }
}

static bool stream_open_chunk(AppVarStream *s) {
    if (s->file) {
        ti_Close(s->file);
        s->file = 0;
    }
    char name[9];
    build_var_name(name, s->slot, s->chunk_idx);

    s->file = ti_Open(name, "r");
    if (!s->file) return false;

    uint16_t size = ti_GetSize(s->file);
    s->ptr = (const uint8_t *)ti_GetDataPtr(s->file);
    s->end = s->ptr + size;
    return true;
}

static inline bool stream_read_bytes(AppVarStream *s, void *dest, size_t count) {
    uint8_t *d = (uint8_t *)dest;
    while (count > 0) {
        if (s->ptr >= s->end) {
            s->chunk_idx++;
            if (!stream_open_chunk(s)) return false;
        }
        size_t avail = s->end - s->ptr;
        size_t take = (count < avail) ? count : avail;
        memcpy(d, s->ptr, take);
        s->ptr += take;
        d += take;
        count -= take;
    }
    return true;
}

static inline uint8_t stream_get_byte(AppVarStream *s) {
    if (s->ptr >= s->end) {
        s->chunk_idx++;
        if (!stream_open_chunk(s)) return 0;
    }
    return *s->ptr++;
}

static inline void draw_pixel_fast(uint8_t x, uint8_t y, uint8_t color) {
    uint8_t *p1 = &gfx_vbuffer[(OFFSET_Y + y * 2) * 320 + (OFFSET_X + x * 2)];
    uint8_t *p2 = p1 + 320;
    p1[0] = color;
    p1[1] = color;
    p2[0] = color;
    p2[1] = color;
}

static void init_grayscale_palette(void) {
    uint16_t palette[16];
    for (uint8_t i = 0; i < 16; i++) {
        uint8_t v = (uint8_t)((i * 255) / 15);
        palette[i] = gfx_RGBTo1555(v, v, v);
    }
    gfx_SetPalette(palette, 16 * sizeof(uint16_t), 0);
}

static void play_video(uint8_t video_slot) {
    AppVarStream stream = {0};
    stream.slot = video_slot;
    stream.chunk_idx = 0;

    if (!stream_open_chunk(&stream)) return;

    char magic[6];
    if (!stream_read_bytes(&stream, magic, 6)) goto cleanup;
    if (memcmp(magic, "CEVID1", 6) != 0) goto cleanup;

    uint16_t width, height;
    uint32_t total_frames;
    if (!stream_read_bytes(&stream, &width, sizeof(uint16_t))) goto cleanup;
    if (!stream_read_bytes(&stream, &height, sizeof(uint16_t))) goto cleanup;
    if (!stream_read_bytes(&stream, &total_frames, sizeof(uint32_t))) goto cleanup;

    init_grayscale_palette();
    gfx_ZeroScreen();

    for (uint32_t f = 0; f < total_frames; f++) {
        kb_Scan();
        if (kb_Data[6] & kb_Clear) break;

        uint8_t frame_type = stream_get_byte(&stream);

        if (frame_type == 0) { // Keyframe (RLE)
            uint32_t rle_len;
            if (!stream_read_bytes(&stream, &rle_len, sizeof(uint32_t))) break;

            uint32_t pixels_drawn = 0;
            uint32_t bytes_read = 0;

            while (bytes_read < rle_len) {
                uint8_t count = stream_get_byte(&stream);
                uint8_t color = stream_get_byte(&stream);
                bytes_read += 2;

                for (uint8_t i = 0; i < count; i++) {
                    uint8_t x = pixels_drawn % width;
                    uint8_t y = pixels_drawn / width;
                    draw_pixel_fast(x, y, color);
                    pixels_drawn++;
                }
            }
        } else if (frame_type == 1) { // Delta frame
            uint16_t num_changes;
            if (!stream_read_bytes(&stream, &num_changes, sizeof(uint16_t))) break;

            for (uint16_t i = 0; i < num_changes; i++) {
                uint8_t x = stream_get_byte(&stream);
                uint8_t y = stream_get_byte(&stream);
                uint8_t color = stream_get_byte(&stream);
                draw_pixel_fast(x, y, color);
            }
        }
    }

cleanup:
    if (stream.file) {
        ti_Close(stream.file);
    }
}

int main(void) {
    uint8_t found_count = 0;
    uint8_t slots[MAX_VIDEOS];

    ti_CloseAll();
    gfx_Begin();
    gfx_SetDrawScreen(); // Direct VRAM target (0 bytes allocated from heap)

    for (uint8_t i = 0; i < MAX_VIDEOS; i++) {
        char name[9];
        build_var_name(name, i, 0);
        ti_var_t f = ti_Open(name, "r");
        if (f) {
            ti_Close(f);
            slots[found_count++] = i;
        }
    }

    if (found_count == 0) {
        init_grayscale_palette();
        gfx_ZeroScreen();
        gfx_SetTextFGColor(15);
        gfx_PrintStringXY("No Video Files Found!", 80, 110);
        gfx_PrintStringXY("Transfer & Archive V0DAT0", 70, 130);
        while (1) {
            kb_Scan();
            if (kb_Data[6] & kb_Clear) break;
        }
        gfx_End();
        ti_CloseAll();
        return 0;
    }

    uint8_t selected_index = 0;

    while (1) {
        init_grayscale_palette();
        gfx_ZeroScreen();
        gfx_SetTextFGColor(15);

        gfx_PrintStringXY("TI-84 CE Video Player", 80, 20);
        gfx_PrintStringXY("---------------------", 80, 32);

        gfx_PrintStringXY("Video Ready to Play", 80, 100);
        gfx_PrintStringXY("Controls:", 40, 140);
        gfx_PrintStringXY("[LEFT / RIGHT] : Switch Video", 40, 160);
        gfx_PrintStringXY("[2nd]          : Play Video", 40, 175);
        gfx_PrintStringXY("[CLEAR]        : Exit App", 40, 190);

        kb_Scan();
        if (kb_Data[7] & kb_Left) {
            if (selected_index > 0) selected_index--;
            else selected_index = found_count - 1;
            while (1) { kb_Scan(); if (!(kb_Data[7] & kb_Left)) break; }
        } else if (kb_Data[7] & kb_Right) {
            if (selected_index < found_count - 1) selected_index++;
            else selected_index = 0;
            while (1) { kb_Scan(); if (!(kb_Data[7] & kb_Right)) break; }
        } else if (kb_Data[1] & kb_2nd) {
            play_video(slots[selected_index]);
            while (1) { kb_Scan(); if (!(kb_Data[1] & kb_2nd)) break; }
        } else if (kb_Data[6] & kb_Clear) {
            break;
        }
    }

    gfx_End();
    ti_CloseAll();
    return 0;
}
