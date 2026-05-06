#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "bsp/device.h"
#include "bootloader_common.h"
#include "esp_system.h"
#include "bsp/display.h"
#include "bsp/audio.h"
#include "driver/i2s_std.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "hal/lcd_types.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "portmacro.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "targets/tanmatsu/tanmatsu_hardware.h"
#include "esp_heap_caps.h"
#include "dirent.h"
#include "sys/stat.h"

// gnuboy headers
#include "gnuboy.h"
#include "loader.h"
void rtc_save(FILE *f);
void rtc_load(FILE *f);
#include "lcd.h"
#include "sound.h"
#include "mem.h"
#include "fb.h"
#include "rc.h"
#include "input.h"
#include "hw.h"
#include "pcm.h"
#include "esp_timer.h"

#include "config.h"
#include "menu.h"
#include "rom_selector.h"

// gnuboy globals required by the emulator core
struct fb  fb;
struct pcm pcm;
int        frame = 0;

// Export tables — stubs to satisfy the linker
rcvar_t emu_exports[] = { RCV_END };
rcvar_t lcd_exports[] = { RCV_END };
rcvar_t vid_exports[] = { RCV_END };
rcvar_t joy_exports[] = { RCV_END };
rcvar_t pcm_exports[] = { RCV_END };

// rckeys stubs — rckeys.c is disabled in the gnuboy component
int  rc_bindkey(char *keyname, char *cmd) { (void)keyname; (void)cmd; return 0; }
int  rc_unbindkey(char *keyname)          { (void)keyname; return 0; }
void rc_unbindall(void)                   {}
void rc_dokey(int key, int st)            { (void)key; (void)st; }

static const char TAG[] = "howboymatsu";

// ── Display globals ───────────────────────────────────────────────────────────
static size_t                       display_h_res        = 0;
static size_t                       display_v_res        = 0;
static lcd_color_rgb_pixel_format_t display_color_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
static lcd_rgb_data_endian_t        display_data_endian  = LCD_RGB_DATA_ENDIAN_LITTLE;
pax_buf_t                           fb_pax               = {0};
QueueHandle_t                       input_event_queue    = NULL;

static uint16_t  gbc_pixels[GBC_WIDTH * GBC_HEIGHT];
uint16_t        *displayBuffer[2]  = {NULL, NULL};
uint8_t         *render_buf_a      = NULL;
static uint8_t  *render_buf_b      = NULL;
static volatile uint8_t active_render_buf = 0;
static SemaphoreHandle_t sem_frame_ready = NULL;
static SemaphoreHandle_t sem_frame_done  = NULL;
TaskHandle_t blit_task_handle     = NULL;
static TaskHandle_t audio_task_handle    = NULL;
static TaskHandle_t emulator_task_handle = NULL;
static SemaphoreHandle_t sem_emulator_done = NULL;

// ── Rewind globals ────────────────────────────────────────────────────────────
static uint8_t  *rewind_state_buf   = NULL;
static uint16_t *rewind_pix_buf     = NULL;
static uint8_t  *rewind_sram_backup = NULL;
static int       rw_sizes[REWIND_SLOTS] = {0};
static int       rw_head        = 0;
static int       rw_count       = 0;
static int       rw_pos         = 0;
static volatile int rewind_active = 0;
static int       rw_frame_ctr   = 0;
static uint16_t  rw_hold_pixels[GBC_WIDTH * GBC_HEIGHT];

// ── Audio globals ─────────────────────────────────────────────────────────────
static int16_t          *audio_buf_a      = NULL;
static int16_t          *audio_buf_b      = NULL;
static volatile int      audio_buf_ready  = 0;
static volatile int      audio_buf_len    = 0;
static SemaphoreHandle_t sem_audio_ready  = NULL;
static SemaphoreHandle_t sem_audio_done   = NULL;
static SemaphoreHandle_t sem_audio_shutdown = NULL;
static float             gbc_volume       = 100.0f;

// ── Misc globals ──────────────────────────────────────────────────────────────
static volatile int  ff_speed           = 0;
static volatile int  return_to_selector = 0;
static volatile int  i2s_enabled        = 1;
static volatile int  audio_mute         = 0;
static bool          show_fps           = false;
static float         current_fps        = 0.0f;
char                 sram_path_global[320] = {0};
char                 state_save_dir[320]   = {0};

// ── Forward declarations ──────────────────────────────────────────────────────
void vid_init(void);
void vid_begin(void);
void pal_dirty(void);
void vid_end(void);
void vid_setpal(int i, int r, int g, int b);
int  pcm_submit(void);
void audio_task(void *arg);
void sys_sleep(int us);
void doevents(void);
void savestate(FILE *f);
void loadstate(FILE *f);
void vram_dirty(void);
void sound_dirty(void);
void mem_updatemap(void);
void rewind_push(void);
void rewind_release_all_keys(void);
int  rewind_pop(void);

// ── Helpers ───────────────────────────────────────────────────────────────────

void restart_to_launcher(void) {
    rtc_retain_mem_t *mem = bootloader_common_get_rtc_retain_mem();
    memset(mem->custom, 0, sizeof(mem->custom));
    esp_restart();
}

void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb_pax));
}

// Blit one physical row, skipping any open menu rect so it persists across frames.
static inline void blit_row(uint16_t *dst, const uint16_t *src, int row,
                             int menu_open, int lm_open, int sm_open) {
    if (menu_open && row >= SS_MENU_R0 && row < SS_MENU_R0 + SS_MENU_RW) {
        memcpy(dst,                    src,                    SS_MENU_COL_LO * 2);
        memcpy(dst + SS_MENU_COL_HI,   src + SS_MENU_COL_HI,  (PHYS_W - SS_MENU_COL_HI) * 2);
    } else if (lm_open && row >= LM_R0 && row < LM_R0 + LM_RW) {
        memcpy(dst,                src,                LM_COL_LO * 2);
        memcpy(dst + LM_COL_HI,    src + LM_COL_HI,   (PHYS_W - LM_COL_HI) * 2);
    } else if (sm_open && row >= SM_R0 && row < SM_R0 + SM_RW) {
        memcpy(dst,                src,                SM_COL_LO * 2);
        memcpy(dst + SM_COL_HI,    src + SM_COL_HI,   (PHYS_W - SM_COL_HI) * 2);
    } else {
        memcpy(dst, src, PHYS_W * 2);
    }
}

// Save SRAM + RTC then signal return to ROM selector.
static void save_sram_and_return_selector(void) {
    if (sram_path_global[0]) {
        FILE *sf = fopen(sram_path_global, "wb");
        if (sf) { sram_save(sf); fclose(sf); ESP_LOGI(TAG, "SRAM saved"); }
        char rtc_path[320];
        strncpy(rtc_path, sram_path_global, sizeof(rtc_path) - 1);
        char *dot = strrchr(rtc_path, '.');
        if (dot) strcpy(dot, ".rtc");
        FILE *rf = fopen(rtc_path, "wb");
        if (rf) { rtc_save(rf); }
    }
    bsp_audio_set_amplifier(false);
    bsp_audio_set_volume(0);
    audio_mute         = 1;
    return_to_selector = 1;
}

// ── GBC screen rendering ──────────────────────────────────────────────────────

static uint16_t scaled_row_565[PHYS_W];

void draw_gbc_screen(void) {
    const int GBC_W  = GBC_WIDTH;
    const int GBC_H  = GBC_HEIGHT;
    const int V_SCALE = 5;

    xSemaphoreTake(sem_frame_done, portMAX_DELAY);
    uint16_t *phys = (uint16_t *)((active_render_buf == 0) ? render_buf_b : render_buf_a);

    static int init_done = 0;
    if (!init_done) {
        memset(render_buf_a, 0, PHYS_W * PHYS_H * 2);
        memset(render_buf_b, 0, PHYS_W * PHYS_H * 2);
        init_done = 1;
    }

    int menu_open = (ss_state == SS_MENU_OPEN || ss_state == SS_MENU_SAVING ||
                     ss_state == SS_MENU_LOADING);
    int lm_open   = layout_menu_open;
    int sm_open   = scale_menu_open;

    // Clear black border bands when scale mode changes (both render buffers).
    if (scale_border_dirty > 0) {
        if (scale_mode == SCALE_FIT) {
            memset(phys,                 0, 133 * PHYS_W * 2);
            memset(phys + 666 * PHYS_W,  0, 134 * PHYS_W * 2);
        } else if (scale_mode == SCALE_3X) {
            memset(phys,                 0, 160 * PHYS_W * 2);
            memset(phys + 640 * PHYS_W,  0, 160 * PHYS_W * 2);
        }
        scale_border_dirty--;
    }

    if (scale_mode == SCALE_FILL) {
        // Stretch 160x144 to 800x480 (5x H, 3.33x V)
        for (int gx = 0; gx < GBC_W; gx++) {
            uint16_t *rp = scaled_row_565;
            for (int gy = GBC_H - 1; gy >= 0; gy--) {
                uint16_t px  = gbc_pixels[gy * GBC_W + gx];
                uint16_t p565 = ((px >> 11) & 0x1F) << 11 |
                                ((px >>  5) & 0x3F) <<  5 |
                                 (px        & 0x1F);
                *rp++ = p565; *rp++ = p565; *rp++ = p565;
                if (gy % 3 == 0) *rp++ = p565;
            }
            int row_start = gx * V_SCALE;
            for (int rep = 0; rep < V_SCALE; rep++)
                blit_row(phys + (row_start + rep) * PHYS_W, scaled_row_565,
                         row_start + rep, menu_open, lm_open, sm_open);
        }
    } else if (scale_mode == SCALE_FIT) {
        // Aspect-correct 533x480, 133 px black bars left/right
        int row = 133;
        for (int gx = 0; gx < GBC_W; gx++) {
            uint16_t *rp = scaled_row_565;
            for (int gy = GBC_H - 1; gy >= 0; gy--) {
                uint16_t px   = gbc_pixels[gy * GBC_W + gx];
                uint16_t p565 = ((px >> 11) & 0x1F) << 11 |
                                ((px >>  5) & 0x3F) <<  5 |
                                 (px        & 0x1F);
                *rp++ = p565; *rp++ = p565; *rp++ = p565;
                if (gy % 3 == 0) *rp++ = p565;
            }
            int row_count = (gx > 0 && gx % 3 == 0) ? 4 : 3;
            for (int rep = 0; rep < row_count; rep++, row++)
                blit_row(phys + row * PHYS_W, scaled_row_565,
                         row, menu_open, lm_open, sm_open);
        }
    } else {
        // Integer 3x scale -> 480x432 centred in 800x480
        memset(scaled_row_565,       0, 24 * sizeof(uint16_t));
        memset(scaled_row_565 + 456, 0, 24 * sizeof(uint16_t));
        for (int gx = 0; gx < GBC_W; gx++) {
            uint16_t *rp = scaled_row_565 + 24;
            for (int gy = GBC_H - 1; gy >= 0; gy--) {
                uint16_t px   = gbc_pixels[gy * GBC_W + gx];
                uint16_t p565 = ((px >> 11) & 0x1F) << 11 |
                                ((px >>  5) & 0x3F) <<  5 |
                                 (px        & 0x1F);
                *rp++ = p565; *rp++ = p565; *rp++ = p565;
            }
            int row_start = gx * 3 + 160;
            for (int rep = 0; rep < 3; rep++)
                blit_row(phys + (row_start + rep) * PHYS_W, scaled_row_565,
                         row_start + rep, menu_open, lm_open, sm_open);
        }
    }

    if (ss_clear_region > 0) ss_clear_region--;
    active_render_buf ^= 1;
    xSemaphoreGive(sem_frame_ready);
}

// ── gnuboy video callbacks ────────────────────────────────────────────────────

void vid_preinit(void) {}

void vid_init(void) {
    displayBuffer[0] = gbc_pixels;
    displayBuffer[1] = gbc_pixels;
    fb.ptr     = (uint8_t *)gbc_pixels;
    fb.w       = GBC_WIDTH;
    fb.h       = GBC_HEIGHT;
    fb.pelsize = 2;
    fb.pitch   = GBC_WIDTH * 2;
    fb.indexed = 0;
    fb.enabled = 1;
    fb.dirty   = 0;
    fb.cc[0].l = 11; fb.cc[0].r = 0;
    fb.cc[1].l = 6;  fb.cc[1].r = 0;
    fb.cc[2].l = 0;  fb.cc[2].r = 0;
    fb.cc[3].l = 0;  fb.cc[3].r = 0;
    ESP_LOGI(TAG, "gnuboy video initialized");
}

void vid_close(void) {}

void vid_begin(void) {
    frame++;
    fb.dirty = 1;
    pal_dirty();
}

void vid_end(void) {
    static int   frame_count = 0;
    static int64_t last_time = 0;
    frame_count++;
    if (last_time == 0) last_time = esp_timer_get_time();

    if (return_to_selector) {
        bsp_audio_set_volume(0);
        if (blit_task_handle) vTaskSuspend(blit_task_handle);
        if (render_buf_a) memset(render_buf_a, 0, PHYS_W * PHYS_H * 2);
        if (render_buf_b) memset(render_buf_b, 0, PHYS_W * PHYS_H * 2);
        bsp_display_blit(0, 0, PHYS_W, PHYS_H, render_buf_a);
        vTaskDelay(pdMS_TO_TICKS(30));
        bsp_display_blit(0, 0, PHYS_W, PHYS_H, render_buf_a);
        if (i2s_enabled) {
            i2s_chan_handle_t i2s_ve = NULL;
            bsp_audio_get_i2s_handle(&i2s_ve);
            if (i2s_ve) { i2s_channel_disable(i2s_ve); i2s_enabled = 0; }
        }
        if (sem_emulator_done) xSemaphoreGive(sem_emulator_done);
        vTaskDelete(NULL);
    }

    // Rewind playback
    if (rewind_active) {
        rw_frame_ctr++;
        if (rw_frame_ctr >= 3) {
            rw_frame_ctr = 0;
            if (!rewind_pop()) {
                // History exhausted — trim buffer and stop
                rw_head       = ((rw_head - rw_pos) % REWIND_SLOTS + REWIND_SLOTS) % REWIND_SLOTS;
                rw_count      = 0;
                rw_pos        = 0;
                rewind_active = 0;
                audio_mute    = 0;
                rewind_release_all_keys();
                if (rewind_sram_backup && ram.sbank && mbc.ramsize > 0) {
                    memcpy(ram.sbank, rewind_sram_backup, (size_t)mbc.ramsize * 8192);
                    ram.sram_dirty = 1;
                }
            }
        } else {
            memcpy(gbc_pixels, rw_hold_pixels, sizeof(gbc_pixels));
        }
        fb.dirty = 1;
        if (fb.dirty) { draw_gbc_screen(); fb.dirty = 0; }
        return;
    }

    // Fast forward frame skipping
    static int ff_frame = 0;
    static const int ff_skip[] = {0, 4, 7};
    int skip = ff_skip[ff_speed];
    if (skip > 0) {
        ff_frame++;
        if (ff_frame <= skip) {
            fb.dirty = 0;
        } else {
            ff_frame = 0;
            if (fb.dirty) { draw_gbc_screen(); fb.dirty = 0; }
        }
    } else {
        ff_frame = 0;
        if (fb.dirty) { draw_gbc_screen(); fb.dirty = 0; }
    }

    // Snapshot for rewind (1x playback only)
    if (ff_speed == 0 && !rewind_active) {
        static int rw_snap_ctr = 0;
        if (++rw_snap_ctr >= REWIND_SNAP_FREQ) {
            rw_snap_ctr = 0;
            rewind_push();
        }
    }

    // Autosave SRAM every ~5 minutes (~18000 frames at 60fps)
    if (frame_count % 18000 == 0 && frame_count > 0 && sram_path_global[0]) {
        FILE *sf = fopen(sram_path_global, "wb");
        if (sf) { sram_save(sf); fclose(sf); ESP_LOGI(TAG, "SRAM autosaved"); }
        char rtc_path[320];
        strncpy(rtc_path, sram_path_global, sizeof(rtc_path) - 1);
        char *dot = strrchr(rtc_path, '.');
        if (dot) strcpy(dot, ".rtc");
        FILE *rf = fopen(rtc_path, "wb");
        if (rf) { rtc_save(rf); }
    }

    if (frame_count % 10 == 0) {
        int64_t now  = esp_timer_get_time();
        current_fps  = 10.0f / ((now - last_time) / 1000000.0f);
        last_time    = now;
        ESP_LOGI(TAG, "FPS: %.1f", current_fps);
        // Refresh palette for first 5 seconds after SRAM load to fix green hue
        if (frame_count < 300 && sram_path_global[0]) {
            pal_dirty();
            vram_dirty();
        }
    }
}

void vid_setpal(int i, int r, int g, int b) { (void)i; (void)r; (void)g; (void)b; }
void vid_settitle(char *title) { ESP_LOGI(TAG, "Game title: %s", title); }

// ── Rewind ────────────────────────────────────────────────────────────────────

void rewind_release_all_keys(void) {
    pad_set(PAD_UP,     0); pad_set(PAD_DOWN,   0);
    pad_set(PAD_LEFT,   0); pad_set(PAD_RIGHT,  0);
    pad_set(PAD_A,      0); pad_set(PAD_B,      0);
    pad_set(PAD_SELECT, 0); pad_set(PAD_START,  0);
}

void rewind_push(void) {
    if (!rewind_state_buf || !rewind_pix_buf) return;
    uint16_t *pslot = rewind_pix_buf + (size_t)rw_head * GBC_WIDTH * GBC_HEIGHT;
    memcpy(pslot, gbc_pixels, sizeof(gbc_pixels));
    uint8_t *sslot = rewind_state_buf + (size_t)rw_head * REWIND_STATE_SZ;
    FILE *sf = fmemopen(sslot, REWIND_STATE_SZ, "wb");
    if (!sf) return;
    savestate(sf);
    rw_sizes[rw_head] = (int)ftell(sf);
    fclose(sf);
    rw_head = (rw_head + 1) % REWIND_SLOTS;
    if (rw_count < REWIND_SLOTS) rw_count++;
}

int rewind_pop(void) {
    if (!rewind_state_buf || !rewind_pix_buf || rw_pos >= rw_count) return 0;
    int idx = ((rw_head - 1 - rw_pos) % REWIND_SLOTS + REWIND_SLOTS) % REWIND_SLOTS;
    uint16_t *pslot = rewind_pix_buf + (size_t)idx * GBC_WIDTH * GBC_HEIGHT;
    memcpy(gbc_pixels,     pslot, sizeof(gbc_pixels));
    memcpy(rw_hold_pixels, pslot, sizeof(gbc_pixels));
    uint8_t *sslot = rewind_state_buf + (size_t)idx * REWIND_STATE_SZ;
    int sz = rw_sizes[idx];
    if (sz <= 0) return 0;
    FILE *lf = fmemopen(sslot, sz, "rb");
    if (!lf) return 0;
    loadstate(lf);
    fclose(lf);
    vram_dirty(); pal_dirty(); sound_dirty(); mem_updatemap();
    rw_pos++;
    return 1;
}

// ── Audio ─────────────────────────────────────────────────────────────────────

void pcm_init(void) {
    static int inited = 0;
    if (inited) {
        if (pcm.buf)     memset(pcm.buf,     0, pcm.len * sizeof(int16_t));
        if (audio_buf_a) memset(audio_buf_a, 0, pcm.len * sizeof(int16_t));
        if (audio_buf_b) memset(audio_buf_b, 0, pcm.len * sizeof(int16_t));
        pcm.pos = 0;
        return;
    }
    inited = 1;
    pcm.hz     = 44100;
    pcm.stereo = 1;
    pcm.len    = (44100 / 60) * 2;
    pcm.buf    = (int16_t *)malloc(pcm.len * sizeof(int16_t));
    audio_buf_a = (int16_t *)malloc(pcm.len * sizeof(int16_t));
    audio_buf_b = (int16_t *)malloc(pcm.len * sizeof(int16_t));
    sem_audio_ready = xSemaphoreCreateBinary();
    sem_audio_done  = xSemaphoreCreateBinary();
    xSemaphoreGive(sem_audio_done);
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 6, &audio_task_handle, 0);
    pcm.pos = 0;
    memset(pcm.buf, 0, pcm.len * sizeof(int16_t));
    ESP_LOGI(TAG, "Audio initialized at %dHz stereo len=%d", pcm.hz, pcm.len);
}

int pcm_submit(void) {
    if (audio_mute || !pcm.buf || pcm.pos == 0 || !sem_audio_ready || !sem_audio_done) {
        pcm.pos = 0;
        return 1;
    }
    if (ff_speed > 0) {
        // Non-blocking silence feed keeps I2S DMA quiet during fast-forward
        if (xSemaphoreTake(sem_audio_done, 0) == pdTRUE) {
            int16_t *ready = (audio_buf_ready == 0) ? audio_buf_a : audio_buf_b;
            memset(ready, 0, pcm.len * sizeof(int16_t));
            audio_buf_len   = pcm.len;
            audio_buf_ready ^= 1;
            xSemaphoreGive(sem_audio_ready);
        }
        pcm.pos = 0;
        return 1;
    }
    xSemaphoreTake(sem_audio_done, pdMS_TO_TICKS(30));
    int16_t *ready = (audio_buf_ready == 0) ? audio_buf_a : audio_buf_b;
    memcpy(ready, pcm.buf, pcm.pos * sizeof(int16_t));
    audio_buf_len   = pcm.pos;
    audio_buf_ready ^= 1;
    pcm.pos = 0;
    xSemaphoreGive(sem_audio_ready);
    return 1;
}

void pcm_close(void) {
    if (pcm.buf) free(pcm.buf);
}

// ── gnuboy system stubs ───────────────────────────────────────────────────────

void  sys_sleep(int us)               { (void)us; }
void *sys_timer(void)                 { return NULL; }
int   sys_elapsed(void *ptr)          { (void)ptr; return 0; }
void  sys_checkdir(char *path, int w) { (void)path; (void)w; }
void  sys_sanitize(char *s)           { (void)s; }
void  sys_initpath(char *exe)         { (void)exe; }

void die(char *fmt, ...) {
    va_list ap;
    char buf[256];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "%s", buf);
    abort();
}

void ev_poll(void) { doevents(); }

// ── Input handling ────────────────────────────────────────────────────────────

void doevents(void) {
    bsp_input_event_t event;
    static uint32_t key_release_time[8] = {0};
    static byte     key_pads[8] = {PAD_A, PAD_B, PAD_START, PAD_SELECT,
                                    PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT};

    // Auto-release timed keys
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    for (int i = 0; i < 4; i++) {
        if (key_release_time[i] > 0 && now >= key_release_time[i]) {
            pad_set(key_pads[i], 0);
            key_release_time[i] = 0;
        }
    }

    // WASD layout: poll scancode state each frame for accurate hold/release
    if (key_layout == 1 && !layout_menu_open) {
        bool st;
        bsp_input_read_scancode(BSP_INPUT_SCANCODE_W,         &st); pad_set(PAD_UP,    (int)st);
        bsp_input_read_scancode(BSP_INPUT_SCANCODE_S,         &st); pad_set(PAD_DOWN,  (int)st);
        bsp_input_read_scancode(BSP_INPUT_SCANCODE_A,         &st); pad_set(PAD_LEFT,  (int)st);
        bsp_input_read_scancode(BSP_INPUT_SCANCODE_D,         &st); pad_set(PAD_RIGHT, (int)st);
        bsp_input_read_scancode(BSP_INPUT_SCANCODE_SEMICOLON, &st); pad_set(PAD_A,     (int)st);
        bsp_input_read_scancode(BSP_INPUT_SCANCODE_LEFTBRACE, &st); pad_set(PAD_B,     (int)st);
    }

    while (xQueueReceive(input_event_queue, &event, 0) == pdTRUE) {
        if (event.type == INPUT_EVENT_TYPE_NAVIGATION) {
            int pressed = event.args_navigation.state;

            // Save state menu input
            if (ss_state == SS_MENU_OPEN && pressed) {
                switch (event.args_navigation.key) {
                    case BSP_INPUT_NAVIGATION_KEY_UP:
                        ss_cursor--;
                        if (ss_cursor < SS_SAVE) ss_cursor = SS_CANCEL;
                        if ((ss_cursor == SS_LOAD || ss_cursor == SS_DELETE) && !ss_exists[ss_slot])
                            ss_cursor = SS_SAVE;
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_DOWN:
                        ss_cursor++;
                        if (ss_cursor > SS_CANCEL) ss_cursor = SS_SAVE;
                        if ((ss_cursor == SS_LOAD || ss_cursor == SS_DELETE) && !ss_exists[ss_slot])
                            ss_cursor = SS_CANCEL;
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_LEFT:
                        ss_slot--;
                        if (ss_slot < 0) ss_slot = 9;
                        if ((ss_cursor == SS_LOAD || ss_cursor == SS_DELETE) && !ss_exists[ss_slot])
                            ss_cursor = SS_SAVE;
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_RIGHT:
                        ss_slot++;
                        if (ss_slot > 9) ss_slot = 0;
                        if ((ss_cursor == SS_LOAD || ss_cursor == SS_DELETE) && !ss_exists[ss_slot])
                            ss_cursor = SS_SAVE;
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_RETURN:
                        if (ss_cursor == SS_CANCEL) {
                            ss_state = SS_MENU_CLOSED;
                        } else if (ss_cursor == SS_SAVE && ss_io_op == 0) {
                            ss_io_op = 1; ss_state = SS_MENU_SAVING; xSemaphoreGive(sem_ss);
                        } else if (ss_cursor == SS_LOAD && ss_exists[ss_slot] && ss_io_op == 0) {
                            ss_io_op = 2; ss_state = SS_MENU_LOADING; xSemaphoreGive(sem_ss);
                        } else if (ss_cursor == SS_DELETE && ss_exists[ss_slot] && ss_io_op == 0) {
                            ss_io_op = 3; ss_state = SS_MENU_SAVING; xSemaphoreGive(sem_ss);
                        }
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_F6:
                        ff_speed = (ff_speed + 1) % 3;
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_F4:
                        ss_state = SS_MENU_CLOSED; break;
                    default: break;
                }
                ss_menu_invalidate();
                continue;
            }

            // Scale menu input
            if (scale_menu_open && pressed) {
                switch (event.args_navigation.key) {
                    case BSP_INPUT_NAVIGATION_KEY_UP:
                        scale_cursor--;
                        if (scale_cursor < 0) scale_cursor = SCALE_COUNT - 1;
                        scale_invalidate(); break;
                    case BSP_INPUT_NAVIGATION_KEY_DOWN:
                        scale_cursor++;
                        if (scale_cursor >= SCALE_COUNT) scale_cursor = 0;
                        scale_invalidate(); break;
                    case BSP_INPUT_NAVIGATION_KEY_RETURN:
                        scale_mode         = scale_cursor;
                        scale_menu_open    = 0;
                        scale_border_dirty = 2;
                        scale_invalidate();
                        ESP_LOGI(TAG, "Scale: %s",
                                 (const char*[]){"STRETCH","FIT","3X"}[scale_mode]);
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_F3:
                        scale_menu_open = 0; scale_invalidate(); break;
                    default: break;
                }
                continue;
            }

            // Layout menu input
            if (layout_menu_open && pressed) {
                switch (event.args_navigation.key) {
                    case BSP_INPUT_NAVIGATION_KEY_UP:
                        lm_cursor = 0; lm_invalidate(); break;
                    case BSP_INPUT_NAVIGATION_KEY_DOWN:
                        lm_cursor = 1; lm_invalidate(); break;
                    case BSP_INPUT_NAVIGATION_KEY_RETURN:
                        key_layout       = lm_cursor;
                        layout_menu_open = 0;
                        lm_invalidate();
                        pad_set(PAD_UP, 0); pad_set(PAD_DOWN, 0);
                        pad_set(PAD_LEFT, 0); pad_set(PAD_RIGHT, 0);
                        key_release_time[4] = key_release_time[5] =
                        key_release_time[6] = key_release_time[7] = 0;
                        ESP_LOGI(TAG, "Layout: %s", key_layout ? "WASD" : "Default");
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_F2:
                        layout_menu_open = 0; lm_invalidate(); break;
                    default: break;
                }
                continue;
            }

            // Normal gameplay keys
            switch (event.args_navigation.key) {
                case BSP_INPUT_NAVIGATION_KEY_UP:    if (key_layout == 0) pad_set(PAD_UP,    pressed); break;
                case BSP_INPUT_NAVIGATION_KEY_DOWN:  if (key_layout == 0) pad_set(PAD_DOWN,  pressed); break;
                case BSP_INPUT_NAVIGATION_KEY_LEFT:  if (key_layout == 0) pad_set(PAD_LEFT,  pressed); break;
                case BSP_INPUT_NAVIGATION_KEY_RIGHT: if (key_layout == 0) pad_set(PAD_RIGHT, pressed); break;
                case BSP_INPUT_NAVIGATION_KEY_RETURN: pad_set(PAD_START, pressed); break;
                case BSP_INPUT_NAVIGATION_KEY_ESC:
                    if (pressed) {
                        if (sram_path_global[0]) {
                            FILE *sf = fopen(sram_path_global, "wb");
                            if (sf) { sram_save(sf); fclose(sf); }
                            char rtc_path[320];
                            strncpy(rtc_path, sram_path_global, sizeof(rtc_path) - 1);
                            char *dot = strrchr(rtc_path, '.');
                            if (dot) strcpy(dot, ".rtc");
                            FILE *rf = fopen(rtc_path, "wb");
                            if (rf) { rtc_save(rf); }
                        }
                        if (blit_task_handle) vTaskSuspend(blit_task_handle);
                        vTaskDelay(pdMS_TO_TICKS(20));
                        if (render_buf_a) {
                            memset(render_buf_a, 0, PHYS_W * PHYS_H * 2);
                            bsp_display_blit(0, 0, PHYS_W, PHYS_H, render_buf_a);
                            vTaskDelay(pdMS_TO_TICKS(50));
                            bsp_display_blit(0, 0, PHYS_W, PHYS_H, render_buf_a);
                        }
                        bsp_audio_set_volume(0);
                        vTaskDelay(pdMS_TO_TICKS(150));
                        restart_to_launcher();
                    }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_VOLUME_UP:
                    if (pressed) {
                        gbc_volume += 5.0f;
                        if (gbc_volume > 100.0f) gbc_volume = 100.0f;
                        bsp_audio_set_volume(gbc_volume);
                    }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN:
                    if (pressed) {
                        gbc_volume -= 5.0f;
                        if (gbc_volume < 0.0f) gbc_volume = 0.0f;
                        bsp_audio_set_volume(gbc_volume);
                    }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_F6:
                    if (pressed) {
                        ff_speed = (ff_speed + 1) % 3;
                        ESP_LOGI(TAG, "FF: %s", (const char*[]){"OFF","5x","8x"}[ff_speed]);
                    }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_F3:
                    if (pressed && ss_state == SS_MENU_CLOSED && !layout_menu_open) {
                        scale_cursor    = scale_mode;
                        scale_menu_open = 1;
                        scale_invalidate();
                    }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_F2:
                    if (pressed && ss_state == SS_MENU_CLOSED) {
                        lm_cursor        = key_layout;
                        layout_menu_open = 1;
                        lm_invalidate();
                    }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_F1:
                    if (pressed && ss_state == SS_MENU_CLOSED && !layout_menu_open) {
                        memset(gbc_pixels, 0, sizeof(gbc_pixels));
                        emu_reset();
                        vram_dirty(); pal_dirty(); sound_dirty(); mem_updatemap();
                        pcm_init();
                        ESP_LOGI(TAG, "Soft reset");
                    }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_F5:
                    if (pressed && ss_state == SS_MENU_CLOSED && !layout_menu_open) {
                        if (!rewind_active) {
                            if (rewind_state_buf && rw_count > 0) {
                                if (rewind_sram_backup && ram.sbank && mbc.ramsize > 0)
                                    memcpy(rewind_sram_backup, ram.sbank, (size_t)mbc.ramsize * 8192);
                                rewind_active = 1;
                                audio_mute    = 1;
                                rw_pos        = 0;
                                rw_frame_ctr  = 2;
                                ESP_LOGI(TAG, "Rewind ON (%d snapshots)", rw_count);
                            }
                        } else {
                            rw_head  = ((rw_head - rw_pos) % REWIND_SLOTS + REWIND_SLOTS) % REWIND_SLOTS;
                            rw_count = (rw_count > rw_pos) ? rw_count - rw_pos : 0;
                            rw_pos        = 0;
                            rewind_active = 0;
                            audio_mute    = 0;
                            rewind_release_all_keys();
                            if (rewind_sram_backup && ram.sbank && mbc.ramsize > 0) {
                                memcpy(ram.sbank, rewind_sram_backup, (size_t)mbc.ramsize * 8192);
                                ram.sram_dirty = 1;
                            }
                            ESP_LOGI(TAG, "Rewind OFF");
                        }
                    }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_F4:
                    if (pressed && ss_state == SS_MENU_CLOSED && !layout_menu_open &&
                        state_save_dir[0]) {
                        for (int si = 0; si < 10; si++) {
                            char spath[340];
                            snprintf(spath, sizeof(spath), "%s.ss%d", state_save_dir, si);
                            struct stat st;
                            ss_exists[si] = (stat(spath, &st) == 0);
                        }
                        ss_cursor   = SS_SAVE;
                        ss_clear_region = 0;
                        ss_state    = SS_MENU_OPEN;
                        ss_menu_invalidate();
                    }
                    break;
                default: break;
            }

        } else if (event.type == INPUT_EVENT_TYPE_KEYBOARD) {
            // Backtick toggles FPS regardless of menu state
            if (event.args_keyboard.ascii == '`') {
                show_fps = !show_fps;
                if (!show_fps) {
                    uint16_t *pa = (uint16_t *)render_buf_a;
                    uint16_t *pb = (uint16_t *)render_buf_b;
                    for (int r = 2; r < 13; r++)
                        for (int c = 2; c < 58; c++) {
                            pa[r * PHYS_W + c] = 0;
                            pb[r * PHYS_W + c] = 0;
                        }
                }
                continue;
            }
            // Layout menu: 'a' confirms, all other keys swallowed
            if (layout_menu_open) {
                if (event.args_keyboard.ascii == 'a' || event.args_keyboard.ascii == 'A') {
                    key_layout       = lm_cursor;
                    layout_menu_open = 0;
                    lm_invalidate();
                    pad_set(PAD_UP, 0); pad_set(PAD_DOWN, 0);
                    pad_set(PAD_LEFT, 0); pad_set(PAD_RIGHT, 0);
                    for (int ki = 4; ki < 8; ki++) key_release_time[ki] = 0;
                    ESP_LOGI(TAG, "Layout: %s", key_layout ? "WASD" : "Default");
                }
                continue;
            }
            uint32_t release_at = now + 100;
            if (key_layout == 0) {
                switch (event.args_keyboard.ascii) {
                    case 'a': case 'A': pad_set(PAD_A, 1);      key_release_time[0] = release_at; break;
                    case 'd': case 'D': pad_set(PAD_B, 1);      key_release_time[1] = release_at; break;
                    case '\n': case '\r': pad_set(PAD_START, 1); key_release_time[2] = release_at; break;
                    case ' ':           pad_set(PAD_SELECT, 1); key_release_time[3] = release_at; break;
                    case '\b': case 127: save_sram_and_return_selector(); break;
                    default: break;
                }
            } else {
                switch (event.args_keyboard.ascii) {
                    case '\n': case '\r': pad_set(PAD_START, 1);  key_release_time[2] = release_at; break;
                    case ' ':            pad_set(PAD_SELECT, 1); key_release_time[3] = release_at; break;
                    case '\b': case 127: save_sram_and_return_selector(); break;
                    default: break;
                }
            }
        }
    }
}

// ── FPS overlay ───────────────────────────────────────────────────────────────

static void draw_fps_overlay(uint8_t *buf) {
    if (!show_fps || current_fps <= 1.0f) return;

    static const uint8_t fps_font[][5] = {
        {0x1F,0x11,0x11,0x11,0x1F},{0x00,0x12,0x1F,0x10,0x00}, // 0,1
        {0x1D,0x15,0x15,0x15,0x17},{0x11,0x15,0x15,0x15,0x1F}, // 2,3
        {0x07,0x04,0x04,0x04,0x1F},{0x17,0x15,0x15,0x15,0x1D}, // 4,5
        {0x1F,0x15,0x15,0x15,0x1D},{0x01,0x01,0x01,0x01,0x1F}, // 6,7
        {0x1F,0x15,0x15,0x15,0x1F},{0x17,0x15,0x15,0x15,0x1F}, // 8,9
        {0x00,0x18,0x18,0x00,0x00},{0x00,0x00,0x00,0x00,0x00}, // ., space
        {0x1F,0x05,0x05,0x05,0x01},{0x1F,0x15,0x15,0x09,0x00}, // F,P
        {0x11,0x15,0x15,0x15,0x1B},                            // S
    };

    char fps_str[16];
    snprintf(fps_str, sizeof(fps_str), "%.1f", current_fps);
    int fps_len = (int)strlen(fps_str);

    uint16_t *phys = (uint16_t *)buf;
    const int SC = 3, start_row = 790;

    for (int ci = 0; ci < fps_len; ci++) {
        char ch = fps_str[ci];
        int idx = -1;
        if (ch >= '0' && ch <= '9') idx = ch - '0';
        else if (ch == '.')  idx = 10;
        else if (ch == ' ')  idx = 11;
        else if (ch == 'F')  idx = 12;
        else if (ch == 'P')  idx = 13;
        else if (ch == 'S')  idx = 14;
        if (idx < 0) continue;

        int char_row = start_row - (fps_len - 1 - ci) * 6 * SC;
        for (int col = 0; col < 5; col++) {
            uint8_t bits = fps_font[idx][col];
            for (int row = 0; row < 7; row++) {
                if (!(bits & (1 << row))) continue;
                for (int sy = 0; sy < SC; sy++)
                for (int sx = 0; sx < SC; sx++) {
                    int px = char_row - (4 - col) * SC - sy;
                    int py = 4 + (6 - row) * SC + sx;
                    if (px >= 0 && px < PHYS_H && py >= 0 && py < PHYS_W)
                        phys[px * PHYS_W + py] = 0xF800;
                }
            }
        }
    }
}

// ── Blit task ─────────────────────────────────────────────────────────────────

void blit_task(void *arg) {
    for (;;) {
        xSemaphoreTake(sem_frame_ready, portMAX_DELAY);
        uint8_t *buf = (active_render_buf == 0) ? render_buf_a : render_buf_b;

        draw_fps_overlay(buf);

        // Save state menu overlay — only redraw when stale (persistent-render pattern)
        if (ss_state == SS_MENU_OPEN || ss_state == SS_MENU_SAVING ||
            ss_state == SS_MENU_LOADING) {
            int dirty = (active_render_buf == 0) ? !ss_menu_drawn_a : !ss_menu_drawn_b;
            if (dirty || ss_toast_f > 0) {
                draw_ss_menu(buf);
                if (active_render_buf == 0) ss_menu_drawn_a = 1;
                else                        ss_menu_drawn_b = 1;
            }
        } else {
            ss_menu_drawn_a = 0;
            ss_menu_drawn_b = 0;
        }

        // Layout menu overlay
        if (layout_menu_open) {
            int dirty = (active_render_buf == 0) ? !lm_drawn_a : !lm_drawn_b;
            if (dirty) {
                draw_layout_menu(buf);
                if (active_render_buf == 0) lm_drawn_a = 1;
                else                        lm_drawn_b = 1;
            }
        } else {
            lm_drawn_a = 0;
            lm_drawn_b = 0;
        }

        // Scale menu overlay
        if (scale_menu_open) {
            int dirty = (active_render_buf == 0) ? !scale_drawn_a : !scale_drawn_b;
            if (dirty) {
                draw_scale_menu(buf);
                if (active_render_buf == 0) scale_drawn_a = 1;
                else                        scale_drawn_b = 1;
            }
        } else {
            scale_drawn_a = 0;
            scale_drawn_b = 0;
        }

        bsp_display_blit(0, 0, PHYS_W, PHYS_H, buf);
        xSemaphoreGive(sem_frame_done);
    }
}

// ── Emulator task ─────────────────────────────────────────────────────────────

void emulator_task(void *arg) {
    static int sd_mounted = 0;
    if (sd_mounted) goto sd_already_mounted;

    ESP_LOGI(TAG, "Mounting SD card...");
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk   = BSP_SDCARD_CLK;
    slot_config.cmd   = BSP_SDCARD_CMD;
    slot_config.d0    = BSP_SDCARD_D0;
    slot_config.d1    = BSP_SDCARD_D1;
    slot_config.d2    = BSP_SDCARD_D2;
    slot_config.d3    = BSP_SDCARD_D3;
    slot_config.width = 4;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        restart_to_launcher();
    }
    ESP_LOGI(TAG, "SD card mounted");
    sd_mounted = 1;
    sd_already_mounted:;

    init_exports();
    rc_command("bind up +up");
    rc_command("bind down +down");
    rc_command("bind left +left");
    rc_command("bind right +right");
    rc_command("bind a +a");
    rc_command("bind b +b");
    rc_command("bind start +start");
    rc_command("bind select +select");

    vid_init();
    pcm_init();
    scan_roms();

    const char *rom_path = NULL;
    if (rom_count == 1) {
        rom_path = rom_list[0];
    } else if (rom_count > 1) {
        return_to_selector = 0;
        ff_speed = 0;
        if (blit_task_handle) vTaskResume(blit_task_handle);
        rom_path = rom_selector();
    }

    if (!rom_path) {
        pax_background(&fb_pax, 0xFF000000);
        pax_draw_text(&fb_pax, 0xFFFF0000, pax_font_sky_mono, 16, 10, 10, "No ROMs found!");
        pax_draw_text(&fb_pax, 0xFFAAAAAA, pax_font_sky_mono, 12, 10, 40, "Place .gbc/.gb in /sdcard/roms/");
        blit();
        bsp_input_event_t ev;
        while (1) {
            if (xQueueReceive(input_event_queue, &ev, portMAX_DELAY) == pdTRUE &&
                ev.type == INPUT_EVENT_TYPE_NAVIGATION &&
                ev.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_ESC &&
                ev.args_navigation.state == 1)
                restart_to_launcher();
        }
    }

    ESP_LOGI(TAG, "Loading ROM: %s", rom_path);
    FILE *rom_fd = fopen(rom_path, "rb");
    if (!rom_fd) {
        ESP_LOGE(TAG, "Cannot open ROM: %s", rom_path);
        pax_background(&fb_pax, 0xFF000000);
        pax_draw_text(&fb_pax, 0xFFFF0000, pax_font_sky_mono, 16, 10, 10, "ROM file not found!");
        pax_draw_text(&fb_pax, 0xFFFFFF00, pax_font_sky_mono, 12, 10, 40, rom_path);
        pax_draw_text(&fb_pax, 0xFFAAAAAA, pax_font_sky_mono, 12, 10, 70, "Press ESC to return");
        blit();
        bsp_input_event_t ev;
        while (1) {
            if (xQueueReceive(input_event_queue, &ev, portMAX_DELAY) == pdTRUE &&
                ev.type == INPUT_EVENT_TYPE_NAVIGATION &&
                ev.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_ESC)
                restart_to_launcher();
        }
    }

    fseek(rom_fd, 0, SEEK_END);
    size_t rom_length = (size_t)ftell(rom_fd);
    fseek(rom_fd, 0, SEEK_SET);
    ESP_LOGI(TAG, "ROM size: %u bytes", (unsigned)rom_length);

    uint8_t *rom_data = (uint8_t *)heap_caps_malloc(rom_length, MALLOC_CAP_SPIRAM);
    if (!rom_data) rom_data = (uint8_t *)malloc(rom_length);
    if (!rom_data) {
        ESP_LOGE(TAG, "Cannot allocate ROM buffer");
        fclose(rom_fd);
        restart_to_launcher();
    }
    fread(rom_data, 1, rom_length, rom_fd);
    fclose(rom_fd);
    loader_init(rom_data);

    // Derive SRAM and state-save paths from ROM filename
    mkdir("/sdcard/saves", 0777);
    const char *rom_base = strrchr(rom_path, '/');
    rom_base = rom_base ? rom_base + 1 : rom_path;
    snprintf(sram_path_global, sizeof(sram_path_global), "/sdcard/saves/%s", rom_base);
    char *dot = strrchr(sram_path_global, '.');
    if (dot) strcpy(dot, ".sav");
    snprintf(state_save_dir, sizeof(state_save_dir), "/sdcard/saves/%s", rom_base);
    char *sdot = strrchr(state_save_dir, '.');
    if (sdot) *sdot = '\0';

    // Load SRAM
    FILE *sram_f = fopen(sram_path_global, "rb");
    if (sram_f) {
        int r = sram_load(sram_f);
        fclose(sram_f);
        char rtc_path[320];
        strncpy(rtc_path, sram_path_global, sizeof(rtc_path) - 1);
        char *rdot = strrchr(rtc_path, '.');
        if (rdot) strcpy(rdot, ".rtc");
        FILE *rtc_f = fopen(rtc_path, "rb");
        if (rtc_f) { rtc_load(rtc_f); } else { rtc_load(NULL); }
        ESP_LOGI(TAG, "SRAM loaded (ret=%d)", r);
    } else {
        int r = sram_load(NULL);
        ESP_LOGI(TAG, "No SRAM save found (ret=%d)", r);
    }

    pax_background(&fb_pax, 0xFF000000);
    blit();

    emu_reset();
    if (sram_path_global[0]) {
        vram_dirty(); pal_dirty(); sound_dirty(); mem_updatemap();
    }
    audio_mute = 0;
    xSemaphoreTake(sem_audio_done, 0);
    xSemaphoreGive(sem_audio_done);
    if (!i2s_enabled) {
        i2s_chan_handle_t i2s_chk = NULL;
        bsp_audio_get_i2s_handle(&i2s_chk);
        if (i2s_chk) { i2s_channel_enable(i2s_chk); i2s_enabled = 1; }
    }
    bsp_audio_set_volume(gbc_volume);
    bsp_audio_set_amplifier(true);
    memset(gbc_pixels, 0, sizeof(gbc_pixels));

    // Allocate rewind PSRAM buffers once — persists across ROM reloads
    if (!rewind_state_buf)
        rewind_state_buf = heap_caps_malloc(
            (size_t)REWIND_SLOTS * REWIND_STATE_SZ, MALLOC_CAP_SPIRAM);
    if (!rewind_pix_buf)
        rewind_pix_buf = heap_caps_malloc(
            (size_t)REWIND_SLOTS * GBC_WIDTH * GBC_HEIGHT * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!rewind_sram_backup)
        rewind_sram_backup = heap_caps_malloc(16 * 8192, MALLOC_CAP_SPIRAM);

    rw_head = 0; rw_count = 0; rw_pos = 0; rw_frame_ctr = 0;
    rewind_active = 0; audio_mute = 0;
    memset(rw_sizes, 0, sizeof(rw_sizes));

    emu_run();
    vTaskDelete(NULL);
}

// ── Audio task ────────────────────────────────────────────────────────────────

void audio_task(void *arg) {
    i2s_chan_handle_t i2s = NULL;
    bsp_audio_get_i2s_handle(&i2s);
    static int16_t silence[4096] = {0};
    size_t written = 0;
    while (1) {
        xSemaphoreTake(sem_audio_ready, portMAX_DELAY);
        if (audio_mute) {
            if (i2s && i2s_enabled)
                i2s_channel_write(i2s, silence, pcm.len * sizeof(int16_t), &written,
                                  pdMS_TO_TICKS(100));
            if (sem_audio_shutdown) xSemaphoreGive(sem_audio_shutdown);
            xSemaphoreGive(sem_audio_done);
            continue;
        }
        int16_t *buf = (audio_buf_ready == 0) ? audio_buf_b : audio_buf_a;
        if (i2s && i2s_enabled)
            i2s_channel_write(i2s, buf, audio_buf_len * sizeof(int16_t), &written,
                              pdMS_TO_TICKS(100));
        xSemaphoreGive(sem_audio_done);
    }
}

// ── App entry point ───────────────────────────────────────────────────────────

void app_main(void) {
    gpio_install_isr_service(0);

    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    const bsp_configuration_t bsp_configuration = {
        .display = {
            .requested_color_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
            .num_fbs = 1,
        },
    };
    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) { ESP_LOGE(TAG, "BSP init failed: %d", res); return; }

    res = bsp_display_get_parameters(&display_h_res, &display_v_res,
                                      &display_color_format, &display_data_endian);
    if (res != ESP_OK) { ESP_LOGE(TAG, "Display params failed: %d", res); return; }

    pax_buf_type_t format = (display_color_format == LCD_COLOR_PIXEL_FORMAT_RGB565)
                            ? PAX_BUF_16_565RGB : PAX_BUF_24_888RGB;

    bsp_display_rotation_t display_rotation = BSP_DISPLAY_ROTATION_90;
    pax_orientation_t orientation = PAX_O_UPRIGHT;
    switch (display_rotation) {
        case BSP_DISPLAY_ROTATION_90:  orientation = PAX_O_ROT_CW;   break;
        case BSP_DISPLAY_ROTATION_180: orientation = PAX_O_ROT_HALF; break;
        case BSP_DISPLAY_ROTATION_270: orientation = PAX_O_ROT_CCW;  break;
        default: break;
    }
    pax_buf_init(&fb_pax, NULL, display_h_res, display_v_res, format);
    pax_buf_reversed(&fb_pax, display_data_endian == LCD_RGB_DATA_ENDIAN_BIG);
    pax_buf_set_orientation(&fb_pax, orientation);

    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    pax_background(&fb_pax, 0xFF000000);
    pax_draw_text(&fb_pax, 0xFFFF0000, pax_font_sky_mono, 24, 10, 10,  "HowBoyMatsu");
    pax_draw_text(&fb_pax, 0xFFFFFFFF, pax_font_sky_mono, 14, 10, 50,  "Game Boy Color Emulator");
    pax_draw_text(&fb_pax, 0xFFAAAAAA, pax_font_sky_mono, 12, 10, 80,  "for Tanmatsu");
    pax_draw_text(&fb_pax, 0xFFFFFF00, pax_font_sky_mono, 12, 10, 120, "Loading ROM...");
    blit();

    vTaskDelay(pdMS_TO_TICKS(500));

    render_buf_a = (uint8_t *)heap_caps_malloc(PHYS_W * PHYS_H * 2, MALLOC_CAP_SPIRAM);
    render_buf_b = (uint8_t *)heap_caps_malloc(PHYS_W * PHYS_H * 2, MALLOC_CAP_SPIRAM);
    if (!render_buf_a || !render_buf_b) {
        ESP_LOGE(TAG, "Failed to allocate render buffers");
        return;
    }
    memset(render_buf_a, 0, PHYS_W * PHYS_H * 2);
    memset(render_buf_b, 0, PHYS_W * PHYS_H * 2);

    sem_frame_ready = xSemaphoreCreateBinary();
    sem_frame_done  = xSemaphoreCreateBinary();
    xSemaphoreGive(sem_frame_done);

    sem_ss = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(ss_io_task, "ss_io",   8192, NULL, 4, NULL,               0);
    xTaskCreatePinnedToCore(blit_task,  "blit",    8192, NULL, 5, &blit_task_handle,  0);

    while (1) {
        sem_emulator_done = xSemaphoreCreateBinary();
        return_to_selector = 0;
        ff_speed = 0;
        if (blit_task_handle) vTaskResume(blit_task_handle);
        xTaskCreatePinnedToCore(emulator_task, "emulator", 32768, NULL, 5,
                                &emulator_task_handle, 1);
        xSemaphoreTake(sem_emulator_done, portMAX_DELAY);
        vSemaphoreDelete(sem_emulator_done);
        sem_emulator_done    = NULL;
        emulator_task_handle = NULL;
        if (!return_to_selector) break;
        if (pcm.buf) { memset(pcm.buf, 0, pcm.len * sizeof(int16_t)); pcm.pos = 0; }
        sound_dirty();
    }
}
