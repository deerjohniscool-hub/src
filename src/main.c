#include <ti/screen.h>
#include <ti/getcsc.h>
#include <sys/lcd.h>
#include <keypadc.h>
#include <graphx.h>
#include <fileioc.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>

#define SRC_WIDTH    160
#define SRC_HEIGHT   120
#define FRAME_SIZE   (SRC_WIDTH * SRC_HEIGHT) // 19,200 bytes

// Expanded buffer to handle worst-case full-frame delta changes
// Total BSS memory: ~40.7 KB (Linker limit: 60.6 KB)
static uint8_t comp_buf[20480];
static uint8_t frame_buf[FRAME_SIZE];
static char log_buf[1024];
static size_t log_pos = 0;

typedef struct {
    uint8_t current_chunk;
    ti_var_t file;
    uint8_t *data;
    size_t size;
    size_t pos;
    char prefix[6];
} ChunkReader;

void log_msg(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (log_pos < sizeof(log_buf) - 128) {
        log_pos += vsnprintf(log_buf + log_pos, sizeof(log_buf) - log_pos, fmt, args);
        log_buf[log_pos++] = '\n';
        log_buf[log_pos] = '\0';
    }
    va_end(args);
}

void display_error(const char *reason) {
    log_msg("FATAL: %s", reason);
    
    ti_var_t log_file = ti_Open("VIDLOG", "w");
    if (log_file) {
        ti_Write(log_buf, 1, log_pos, log_file);
        ti_Close(log_file);
    }

    gfx_End();
    os_ClrHome();
    os_PutStrFull("VIDEO PLAYER ERROR:");
    os_NewLine();
    os_PutStrFull(reason);
    os_NewLine();
    os_NewLine();
    os_PutStrFull("Press any key to exit...");
    
    while (!os_GetCSC());
}

bool open_chunk_ptr(ChunkReader *r, uint8_t chunk_idx) {
    if (r->file) {
        ti_Close(r->file);
        r->file = 0;
    }
    
    char var_name[10];
    snprintf(var_name, sizeof(var_name), "%s%u", r->prefix, chunk_idx);
    
    r->file = ti_Open(var_name, "r");
    if (!r->file) return false;
    
    r->data = (uint8_t *)ti_GetDataPtr(r->file);
    r->size = ti_GetSize(r->file);
    r->pos = 0;
    r->current_chunk = chunk_idx;
    log_msg("Opened chunk %s (%u bytes)", var_name, (unsigned int)r->size);
    return true;
}

bool read_bytes_safe(ChunkReader *r, void *dest, size_t count) {
    size_t read_so_far = 0;
    uint8_t *out = (uint8_t *)dest;
    
    while (read_so_far < count) {
        if (r->pos >= r->size) {
            if (!open_chunk_ptr(r, r->current_chunk + 1)) {
                return false;
            }
        }
        size_t available = r->size - r->pos;
        size_t to_read = count - read_so_far;
        if (to_read > available) to_read = available;
        
        memcpy(out + read_so_far, r->data + r->pos, to_read);
        r->pos += to_read;
        read_so_far += to_read;
    }
    return true;
}

static void decompress_rle_delta(const uint8_t *in, size_t in_len, uint8_t *out_frame, size_t out_len) {
    size_t in_idx = 0;
    size_t out_idx = 0;
    
    while (in_idx < in_len && out_idx < out_len) {
        uint8_t count = in[in_idx++];
        if (count & 0x80) {
            uint8_t run_len = (count & 0x7F) + 1;
            if (in_idx >= in_len) break;
            uint8_t val = in[in_idx++];
            for (uint8_t i = 0; i < run_len && out_idx < out_len; i++) {
                out_frame[out_idx++] ^= val;
            }
        } else {
            uint8_t lit_len = count + 1;
            for (uint8_t i = 0; i < lit_len && in_idx < in_len && out_idx < out_len; i++) {
                out_frame[out_idx++] ^= in[in_idx++];
            }
        }
    }
}

void render_frame_scaled_2x(const uint8_t *src) {
    uint8_t *vbuf = gfx_vbuffer;

    for (uint16_t y = 0; y < SRC_HEIGHT; y++) {
        uint8_t *row1 = vbuf + (y * 2) * 320;
        uint8_t *row2 = row1 + 320;
        const uint8_t *s_row = src + y * SRC_WIDTH;

        for (uint16_t x = 0; x < SRC_WIDTH; x++) {
            uint8_t pixel = s_row[x];
            row1[0] = pixel; row1[1] = pixel;
            row2[0] = pixel; row2[1] = pixel;
            row1 += 2;
            row2 += 2;
        }
    }
}

void setup_grayscale_palette(void) {
    for (int i = 0; i < 256; i++) {
        uint8_t r = i >> 3;
        uint8_t g = i >> 2;
        uint8_t b = i >> 3;
        gfx_palette[i] = (1 << 15) | (r << 10) | (g << 5) | b;
    }
}

void play_video(const char *prefix) {
    ChunkReader reader;
    memset(&reader, 0, sizeof(reader));
    strncpy(reader.prefix, prefix, 5);
    
    log_msg("Starting playback: %s", reader.prefix);
    
    if (!open_chunk_ptr(&reader, 0)) {
        display_error("Could not find AppVar chunk 0");
        return;
    }
    
    char magic[6];
    uint16_t width = 0, height = 0;
    uint32_t total_frames = 0;
    
    if (!read_bytes_safe(&reader, magic, 6) ||
        !read_bytes_safe(&reader, &width, 2) ||
        !read_bytes_safe(&reader, &height, 2) ||
        !read_bytes_safe(&reader, &total_frames, 4)) {
        display_error("Failed to read stream header");
        return;
    }
    
    if (memcmp(magic, "CEVID1", 6) != 0) {
        display_error("Magic header mismatch");
        return;
    }
    
    if (width != SRC_WIDTH || height != SRC_HEIGHT) {
        display_error("Resolution mismatch (Expected 160x120)");
        return;
    }
    
    gfx_Begin();
    gfx_SetDrawBuffer();
    setup_grayscale_palette();
    
    memset(frame_buf, 0, sizeof(frame_buf));
    
    for (uint32_t frame = 0; frame < total_frames; frame++) {
        kb_Scan();
        if (kb_IsDown(kb_KeyClear) || kb_IsDown(kb_KeyMode)) break;
        
        uint16_t comp_len = 0;
        if (!read_bytes_safe(&reader, &comp_len, 2)) {
            display_error("EOF reading frame size");
            return;
        }
        
        if (comp_len > sizeof(comp_buf)) {
            display_error("Frame exceeds buffer limit");
            return;
        }
        
        if (!read_bytes_safe(&reader, comp_buf, comp_len)) {
            display_error("EOF reading frame data");
            return;
        }
        
        decompress_rle_delta(comp_buf, comp_len, frame_buf, sizeof(frame_buf));
        render_frame_scaled_2x(frame_buf);
        gfx_SwapDraw();
    }
    
    gfx_End();
    if (reader.file) ti_Close(reader.file);
}

int main(void) {
    os_ClrHome();
    play_video("V0DAT");
    return 0;
}
