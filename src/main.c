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
#define FRAME_WIDTH 120
#define FRAME_HEIGHT 90
#define FRAME_PACKED_SIZE ((FRAME_WIDTH * FRAME_HEIGHT) / 2) // 5,400 bytes
#define FRAMES_PER_CHUNK 3
#define MAX_VIDEOS 10

static char var_name[9];
static uint16_t gray_palette[16];
static uint8_t slots[MAX_VIDEOS];

static void build_var_name(uint8_t slot, uint8_t chunk) {
    var_name[0] = 'V';
    var_name[1] = '0' + (slot % 10);
    var_name[2] = 'D';
    var_name[3] = 'A';
    var_name[4] = 'T';
    if (chunk < 10) {
        var_name[5] = '0' + chunk;
        var_name[6] = '\0';
    } else {
        var_name[5] = '0' + (chunk / 10);
        var_name[6] = '0' + (chunk % 10);
        var_name[7] = '\0';
    }
}

static void init_grayscale_palette(void) {
    for (uint8_t i = 0; i < 16; i++) {
        uint8_t v = (uint8_t)((i * 255) / 15);
        gray_palette[i] = gfx_RGBTo1555(v, v, v);
    }
    gfx_SetPalette(gray_palette, 16 * sizeof(uint16_t), 0);
}

static void show_error(const char *msg1, const char *msg2) {
    gfx_SetDrawScreen();
    init_grayscale_palette();
    gfx_FillScreen(0);
    gfx_SetTextFGColor(15);
    gfx_PrintStringXY(msg1, 20, 100);
    if (msg2) gfx_PrintStringXY(msg2, 20, 120);
    gfx_PrintStringXY("Press CLEAR to return", 20, 160);

    while (1) {
        kb_Scan();
        if (kb_Data[6] & kb_Clear) break;
    }
    while (1) {
        kb_Scan();
        if (!(kb_Data[6] & kb_Clear)) break;
    }
    gfx_SetDrawBuffer();
}

static inline bool check_exit_key(void) {
    kb_Scan();
    return (kb_Data[6] & kb_Clear) != 0;
}

static void play_video(uint8_t video_slot) {
    ti_var_t file;
    uint8_t *ptr;
    uint16_t var_size = 0;
    uint16_t width = 0, height = 0;
    uint8_t target_fps = 0, num_colors = 0;
    uint32_t total_frames = 0;
    uint8_t current_chunk = 0;
    uint8_t frame_in_chunk = 0;

    ti_CloseAll();
    build_var_name(video_slot, 0);

    file = ti_Open(var_name, "r");
    if (!file) {
        show_error("Error: AppVar Not Found", "Archive V0DAT files to Flash!");
        return;
    }

    var_size = ti_GetSize(file);
    ptr = (uint8_t *)ti_GetDataPtr(file);
    if (!ptr || var_size < 48) {
        ti_CloseAll();
        show_error("Error: Corrupt Header", "AppVar size is invalid");
        return;
    }

    if (memcmp(ptr, "CEVID2", 6) != 0) {
        ti_CloseAll();
        show_error("Error: Header Mismatch", "Re-convert with updated Converter.py");
        return;
    }
    ptr += 6;

    memcpy(&width, ptr, 2); ptr += 2;
    memcpy(&height, ptr, 2); ptr += 2;
    target_fps = *ptr++;
    num_colors = *ptr++;

    if (width != FRAME_WIDTH || height != FRAME_HEIGHT) {
        ti_CloseAll();
        show_error("Error: Invalid Specs", "Expected 120x90 resolution");
        return;
    }

    ptr += num_colors * sizeof(uint16_t);
    memcpy(&total_frames, ptr, 4);
    ptr += 4;

    init_grayscale_palette();
    gfx_ZeroScreen();

    uint8_t *chunk_end = ((uint8_t *)ti_GetDataPtr(file)) + var_size;

    for (uint32_t f = 0; f < total_frames; f++) {
        if (frame_in_chunk >= FRAMES_PER_CHUNK) {
            ti_CloseAll();
            current_chunk++;
            frame_in_chunk = 0;

            build_var_name(video_slot, current_chunk);
            file = ti_Open(var_name, "r");
            if (!file) break;

            var_size = ti_GetSize(file);
            ptr = (uint8_t *)ti_GetDataPtr(file);
            if (!ptr || var_size < FRAME_PACKED_SIZE) break;
            
            chunk_end = ptr + var_size;
        }

        if (ptr + FRAME_PACKED_SIZE > chunk_end) {
            break; // Prevents reading past memory boundary
        }

        uint8_t *frame_src = ptr;
        ptr += FRAME_PACKED_SIZE;
        frame_in_chunk++;

        // Render to backbuffer
        for (uint8_t y = 0; y < FRAME_HEIGHT; y++) {
            uint8_t *line_ptr = &gfx_vbuffer[(OFFSET_Y + (y << 1)) * 320 + OFFSET_X];

            for (uint8_t x = 0; x < FRAME_WIDTH; x += 2) {
                uint8_t val = *frame_src++;
                uint8_t c1 = val >> 4;
                uint8_t c2 = val & 0x0F;

                line_ptr[0] = c1; line_ptr[1] = c1;
                line_ptr[320] = c1; line_ptr[321] = c1;

                line_ptr[2] = c2; line_ptr[3] = c2;
                line_ptr[322] = c2; line_ptr[323] = c2;

                line_ptr += 4;
            }
        }

        gfx_BlitBuffer();

        if (check_exit_key()) break;
    }

    ti_CloseAll();
}

int main(void) {
    uint8_t found_count = 0;
    uint8_t selected_index = 0;

    ti_CloseAll();
    gfx_Begin();
    gfx_SetDrawBuffer();

    for (uint8_t i = 0; i < MAX_VIDEOS; i++) {
        ti_var_t f;
        build_var_name(i, 0);
        f = ti_Open(var_name, "r");
        if (f) {
            ti_Close(f);
            slots[found_count++] = i;
        }
    }

    if (found_count == 0) {
        show_error("No Video Files Found!", "Transfer & Archive V0DAT0");
        gfx_End();
        ti_CloseAll();
        return 0;
    }

    while (1) {
        init_grayscale_palette();
        gfx_ZeroScreen();
        gfx_SetTextFGColor(15);

        gfx_PrintStringXY("TI-84 CE Grayscale Video Player", 35, 20);
        gfx_PrintStringXY("-------------------------------", 35, 32);

        gfx_PrintStringXY("Video Ready to Play", 40, 100);
        gfx_PrintStringXY("Controls:", 40, 140);
        gfx_PrintStringXY("[LEFT / RIGHT] : Switch Slot", 40, 160);
        gfx_PrintStringXY("[2nd]          : Play Video", 40, 175);
        gfx_PrintStringXY("[CLEAR]        : Exit App", 40, 190);

        gfx_BlitBuffer();

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
