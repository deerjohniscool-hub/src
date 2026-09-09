#include <ti/screen.h>
#include <fileioc.h>
#include <graphx.h>
#include <keypadc.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#define OFFSET_X 40
#define OFFSET_Y 30
#define FRAME_WIDTH 120
#define FRAME_HEIGHT 90
#define FRAME_PACKED_SIZE ((FRAME_WIDTH * FRAME_HEIGHT) / 2) // 5,400 bytes
#define FRAMES_PER_CHUNK 3
#define MAX_VIDEOS 10

void set_ui_palette(void) {
    uint16_t ui_palette[2] = {
        gfx_RGBTo1555(15, 15, 25),   // 0: Dark Background
        gfx_RGBTo1555(255, 255, 255) // 1: Bright White Text
    };
    gfx_SetPalette(ui_palette, sizeof(ui_palette), 0);
}

void show_error(const char *msg1, const char *msg2) {
    set_ui_palette();
    gfx_FillScreen(0);
    gfx_SetTextFGColor(1);
    gfx_PrintStringXY(msg1, 20, 100);
    if (msg2) gfx_PrintStringXY(msg2, 20, 120);
    gfx_PrintStringXY("Press CLEAR to return", 20, 160);
    gfx_BlitBuffer();
    
    while (1) {
        kb_Scan();
        if (kb_Data[6] & kb_Clear) break;
    }
}

static bool wait_frame_and_check_exit(uint8_t fps) {
    uint32_t delay_count = 90000 / (fps ? fps : 12);
    for (volatile uint32_t i = 0; i < delay_count; i++) {
        if ((i & 0x1FF) == 0) {
            kb_Scan();
            if (kb_Data[6] & kb_Clear) {
                return true;
            }
        }
    }
    return false;
}

void play_video(uint8_t video_slot) {
    ti_CloseAll();

    char var_name[10];
    snprintf(var_name, sizeof(var_name), "V%uDAT0", video_slot);

    ti_var_t file = ti_Open(var_name, "r");
    if (!file) {
        show_error("Error: Could not open chunk 0", "Archive V0DAT files to Flash!");
        return;
    }

    uint8_t *ptr = (uint8_t *)ti_GetDataPtr(file);
    if (!ptr) {
        ti_CloseAll();
        show_error("Error: Invalid AppVar pointer", "Re-transfer video files");
        return;
    }

    if (memcmp(ptr, "CEVID2", 6) != 0) {
        ti_CloseAll();
        show_error("Error: Header Mismatch", "Re-convert with updated Converter.py");
        return;
    }
    ptr += 6;

    uint16_t width = 0, height = 0;
    uint8_t target_fps = 0, num_colors = 0;
    uint32_t total_frames = 0;

    memcpy(&width, ptr, 2); ptr += 2;
    memcpy(&height, ptr, 2); ptr += 2;
    target_fps = *ptr++;
    num_colors = *ptr++;

    if (width != FRAME_WIDTH || height != FRAME_HEIGHT || num_colors > 16) {
        ti_CloseAll();
        show_error("Error: Invalid dimensions", "Expected 120x90, 16 colors");
        return;
    }

    uint16_t palette[16];
    memcpy(palette, ptr, num_colors * sizeof(uint16_t));
    ptr += num_colors * sizeof(uint16_t);

    memcpy(&total_frames, ptr, 4);
    ptr += 4;

    gfx_SetPalette(palette, num_colors * sizeof(uint16_t), 0);
    gfx_FillScreen(0);

    uint8_t current_chunk = 0;
    uint8_t frame_in_chunk = 0;

    for (uint32_t f = 0; f < total_frames; f++) {
        if (frame_in_chunk >= FRAMES_PER_CHUNK) {
            ti_CloseAll();
            current_chunk++;
            frame_in_chunk = 0;

            snprintf(var_name, sizeof(var_name), "V%uDAT%u", video_slot, current_chunk);
            file = ti_Open(var_name, "r");
            if (!file) break;

            ptr = (uint8_t *)ti_GetDataPtr(file);
            if (!ptr) break;
        }

        uint8_t *frame_src = ptr;
        ptr += FRAME_PACKED_SIZE;
        frame_in_chunk++;

        for (uint8_t y = 0; y < FRAME_HEIGHT; y++) {
            uint8_t *line_ptr = &gfx_vbuffer[(OFFSET_Y + (y << 1)) * 320 + OFFSET_X];

            for (uint8_t x = 0; x < FRAME_WIDTH; x += 2) {
                uint8_t val = *frame_src++;
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

        gfx_BlitBuffer();

        if (wait_frame_and_check_exit(target_fps)) {
            break;
        }
    }

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
        show_error("No Video Files Found!", "Transfer & Archive V0DAT0");
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

        kb_Scan();
        if (kb_Data[7] & kb_Left) {
            if (selected_index > 0) selected_index--;
            else selected_index = found_count - 1;
            while (kb_Data[7] & kb_Left) kb_Scan();
        } else if (kb_Data[7] & kb_Right) {
            if (selected_index < found_count - 1) selected_index++;
            else selected_index = 0;
            while (kb_Data[7] & kb_Right) kb_Scan();
        } else if (kb_Data[1] & kb_2nd) {
            play_video(slots[selected_index]);
            while (kb_Data[1] & kb_2nd) kb_Scan();
        } else if (kb_Data[6] & kb_Clear) {
            break;
        }
    }

    gfx_End();
    ti_CloseAll();
    return 0;
}
