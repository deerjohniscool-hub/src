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
#include <stdarg.h>

#define SCALE_FACTOR 2
#define FRAME_DELAY_MS 50
#define MAX_VIDEOS 10
#define MAX_PIXELS (160 * 120)

typedef struct {
    ti_var_t file;
    uint8_t *data;
    size_t size;
    size_t pos;
    uint8_t current_chunk;
    char base_name[7];
} ChunkPointerReader;

static uint8_t frame_mem[MAX_PIXELS];
static uint16_t global_palette[256];
static ChunkPointerReader reader;
static char log_buffer[2048];
static size_t log_len = 0;

void log_init(void) {
    log_len = 0;
    memset(log_buffer, 0, sizeof(log_buffer));
}

void log_msg(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (log_len < sizeof(log_buffer) - 128) {
        log_len += vsnprintf(log_buffer + log_len, sizeof(log_buffer) - log_len, fmt, args);
        log_buffer[log_len++] = '\n';
    }
    va_end(args);
}

void save_log_appvar(void) {
    ti_var_t log_file = ti_Open("VIDLOG", "w");
    if (log_file) {
        ti_Write(log_buffer, 1, log_len, log_file);
        ti_Close(log_file);
    }
}

void display_error(const char *reason) {
    log_msg("CRITICAL ERROR: %s", reason);
    save_log_appvar();

    gfx_ZeroScreen();
    gfx_SetTextFGColor(255);
    gfx_PrintStringXY("PLAYER ERROR DETECTED", 20, 20);
    gfx_PrintStringXY("---------------------", 20, 32);
    gfx_PrintStringXY("Reason:", 20, 50);
    gfx_PrintStringXY(reason, 20, 65);
    gfx_PrintStringXY("Log saved to AppVar: VIDLOG", 20, 160);
    gfx_PrintStringXY("Press [CLEAR] to return", 20, 190);
    gfx_BlitBuffer();

    while (os_GetCSC() != sk_Clear);
}

void setup_grayscale_palette(void) {
    memset(global_palette, 0, sizeof(global_palette));
    for (int i = 0; i < 16; i++) {
        uint8_t level = (i * 255) / 15;
        global_palette[i] = gfx_RGBTo1555(level, level, level);
    }
    global_palette[255] = gfx_RGBTo1555(255, 255, 255);
    gfx_SetPalette(global_palette, sizeof(global_palette), 0);
}

bool open_chunk_ptr(ChunkPointerReader *r, uint8_t chunk_idx) {
    if (r->file) {
        ti_Close(r->file);
        r->file = 0;
    }
    char var_name[9];
    snprintf(var_name, sizeof(var_name), "%s%u", r->base_name, chunk_idx);
    r->file = ti_Open(var_name, "r");
    if (!r->file) return false;
    
    r->data = (uint8_t *)ti_GetDataPtr(r->file);
    r->size = ti_GetSize(r->file);
    r->pos = 0;
    r->current_chunk = chunk_idx;
    log_msg("Opened chunk %s (Size: %u bytes)", var_name, r->size);
    return (r->data != NULL && r->size > 0);
}

bool read_bytes_safe(ChunkPointerReader *r, void *dest, size_t bytes_to_read) {
    uint8_t *out = (uint8_t *)dest;
    size_t bytes_read = 0;

    while (bytes_read < bytes_to_read) {
        if (!r->data || r->pos >= r->size) {
            if (!open_chunk_ptr(r, r->current_chunk + 1)) {
                return false;
            }
        }
        
        size_t available = r->size - r->pos;
        size_t needed = bytes_to_read - bytes_read;
        size_t take = (needed < available) ? needed : available;
        
        memcpy(out + bytes_read, r->data + r->pos, take);
        r->pos += take;
        bytes_read += take;
    }
    return true;
}

void render_frame_scaled(const uint8_t *src, uint16_t w, uint16_t h) {
    uint8_t *vbuf = gfx_GetDraw();
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

bool delay_or_exit(uint16_t ms) {
    uint16_t elapsed = 0;
    while (elapsed < ms) {
        if (os_GetCSC() == sk_Clear) return true;
        delay(5);
        elapsed += 5;
    }
    return false;
}

void play_video(uint8_t video_slot) {
    log_init();
    log_msg("Starting playback slot V%uDAT", video_slot);

    memset(&reader, 0, sizeof(ChunkPointerReader));
    snprintf(reader.base_name, sizeof(reader.base_name), "V%uDAT", video_slot);

    if (!open_chunk_ptr(&reader, 0)) {
        display_error("Could not open initial chunk 0");
        return;
    }

    char magic[6];
    if (!read_bytes_safe(&reader, magic, 6)) {
        display_error("Failed to read header magic");
        return;
    }
    
    if (memcmp(magic, "CEVID1", 6) != 0) {
        char err_buf[64];
        snprintf(err_buf, sizeof(err_buf), "Header mismatch: '%.6s'", magic);
        display_error(err_buf);
        return;
    }

    uint16_t width = 0, height = 0;
    uint32_t total_frames = 0;

    if (!read_bytes_safe(&reader, &width, 2) ||
        !read_bytes_safe(&reader, &height, 2) ||
        !read_bytes_safe(&reader, &total_frames, 4)) {
        display_error("Failed reading video metadata");
        return;
    }

    log_msg("Metadata: %ux%u, Frames: %u", width, height, total_frames);

    if (width > 160 || height > 120) {
        display_error("Resolution exceeds 160x120 limit");
        return;
    }

    setup_grayscale_palette();
    gfx_ZeroScreen();
    memset(frame_mem, 0, sizeof(frame_mem));

    for (uint32_t f = 0; f < total_frames; f++) {
        if (os_GetCSC() == sk_Clear) break;

        uint8_t frame_type;
        if (!read_bytes_safe(&reader, &frame_type, 1)) {
            display_error("Unexpected EOF reading frame type");
            return;
        }

        if (frame_type == 0) { 
            uint32_t rle_len = 0;
            if (!read_bytes_safe(&reader, &rle_len, 4)) {
                display_error("Failed reading RLE len");
                return;
            }

            uint32_t pixels_drawn = 0;
            uint32_t bytes_read = 0;

            while (bytes_read < rle_len) {
                uint8_t count = 0, color = 0;
                if (!read_bytes_safe(&reader, &count, 1) ||
                    !read_bytes_safe(&reader, &color, 1)) {
                    display_error("EOF during RLE stream");
                    return;
                }
                bytes_read += 2;

                for (uint8_t i = 0; i < count; i++) {
                    if (pixels_drawn < MAX_PIXELS) {
                        frame_mem[pixels_drawn++] = color;
                    } else {
                        display_error("RLE overflowed 160x120 buffer");
                        return;
                    }
                }
            }
        } else if (frame_type == 1) { 
            uint16_t num_changes = 0;
            if (!read_bytes_safe(&reader, &num_changes, 2)) {
                display_error("Failed reading delta frame count");
                return;
            }

            for (uint16_t i = 0; i < num_changes; i++) {
                uint8_t x = 0, y = 0, color = 0;
                if (!read_bytes_safe(&reader, &x, 1) ||
                    !read_bytes_safe(&reader, &y, 1) ||
                    !read_bytes_safe(&reader, &color, 1)) {
                    display_error("EOF inside delta frame");
                    return;
                }
                
                if (x < width && y < height) {
                    uint32_t idx = (uint32_t)y * width + x;
                    if (idx < MAX_PIXELS) {
                        frame_mem[idx] = color;
                    }
                } else {
                    display_error("Delta pixel out of bounds");
                    return;
                }
            }
        }

        render_frame_scaled(frame_mem, width, height);
        gfx_BlitBuffer();

        if (delay_or_exit(FRAME_DELAY_MS)) break;
    }

    if (reader.file) {
        ti_Close(reader.file);
        reader.file = 0;
    }
}

int main(void) {
    gfx_Begin();
    gfx_SetDrawBuffer();
    setup_grayscale_palette();

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
