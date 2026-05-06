#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "bsp/device.h"
#include "bootloader_common.h"
#include "esp_system.h"
#include "bsp/display.h"
#include "bsp/audio.h"
#include "driver/i2s_std.h"
#include "bsp/audio.h"
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
void rtc_save(FILE* f);
void rtc_load(FILE* f);
#include "lcd.h"
#include "sound.h"
#include "mem.h"
#include "fb.h"
#include "rc.h"
#include "input.h"
#include "hw.h"
#include "pcm.h"
#include "lcd.h"
#include "esp_timer.h"

// gnuboy global variables required by the emulator core
struct fb fb;
struct pcm pcm;
int frame = 0;

// Display buffers gnuboy uses internally
static uint16_t displayBuffer0[160 * 144];
static uint16_t displayBuffer1[160 * 144];
uint16_t* displayBuffer[2] = { displayBuffer0, displayBuffer1 };
uint8_t currentBuffer = 0;

// Frame buffer that gnuboy's lcd.c writes scan lines into
static uint8_t *display_buf = NULL;
static uint8_t *render_buf_a = NULL;
static uint8_t *render_buf_b = NULL;
static volatile uint8_t active_render_buf = 0;
static SemaphoreHandle_t sem_frame_ready = NULL;
static SemaphoreHandle_t sem_frame_done = NULL;
static TaskHandle_t blit_task_handle = NULL;
static TaskHandle_t audio_task_handle = NULL;
static TaskHandle_t emulator_task_handle = NULL;
static SemaphoreHandle_t sem_emulator_done = NULL;
static volatile bool sram_load_done = false;

// Export tables - stubs to satisfy the linker
rcvar_t emu_exports[] = { RCV_END };
rcvar_t lcd_exports[] = { RCV_END };
rcvar_t vid_exports[] = { RCV_END };
rcvar_t joy_exports[] = { RCV_END };
rcvar_t pcm_exports[] = { RCV_END };

// rckeys stubs - rckeys.c is disabled in the gnuboy component
#define MAX_KEYS 256
static char *keybind[MAX_KEYS];
int rc_bindkey(char *keyname, char *cmd)   { (void)keyname; (void)cmd; return 0; }
int rc_unbindkey(char *keyname)            { (void)keyname; return 0; }
void rc_unbindall(void)                    {}
void rc_dokey(int key, int st)             { (void)key; (void)st; }

// Constants
static char const TAG[] = "howboymatsu";

// ROM path on SD card


// GBC screen size
#define GBC_WIDTH  160
#define GBC_HEIGHT 144

// Display scale factor
#define SCALE 5

// Global variables
static size_t                       display_h_res        = 0;
static size_t                       display_v_res        = 0;
static lcd_color_rgb_pixel_format_t display_color_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
static lcd_rgb_data_endian_t        display_data_endian  = LCD_RGB_DATA_ENDIAN_LITTLE;
static pax_buf_t                    fb_pax               = {0};
static QueueHandle_t                input_event_queue    = NULL;

// gnuboy pixel buffer (RGB565, 160x144)
static uint16_t gbc_pixels[GBC_WIDTH * GBC_HEIGHT];

// Forward declarations
void vid_init(void);
void vid_begin(void);
void pal_dirty(void);
void vid_end(void);
void vid_setpal(int i, int r, int g, int b);
int  pcm_submit(void);
void audio_task(void *arg);
void sys_sleep(int us);
static void restart_to_launcher(void) {
    rtc_retain_mem_t* mem = bootloader_common_get_rtc_retain_mem();
    memset(mem->custom, 0, sizeof(mem->custom));
    esp_restart();
}

void doevents(void);
void savestate(FILE *f);
void loadstate(FILE *f);
void vram_dirty(void);
void sound_dirty(void);
void mem_updatemap(void);
// Blit PAX framebuffer to display

static void blit(void);
static char sram_path_global[320] = {0};
static char state_save_dir[320]   = {0};

// Save state menu
#define SS_MENU_CLOSED  0
#define SS_MENU_OPEN    1
#define SS_MENU_SAVING  2
#define SS_MENU_LOADING 3
#define SS_SAVE   0
#define SS_LOAD   1
#define SS_DELETE 2
#define SS_CANCEL 3

// Menu rect in PAX coords. Must match draw_ss_menu's R0/RW/C0/BH.
// vid_end uses these to skip the menu region when the menu is open,
// so the menu draw cost only pays off on first frame after invalidation.
#define SS_MENU_R0   560
#define SS_MENU_RW   220
#define SS_MENU_C0   460
#define SS_MENU_BH   190
#define SS_MENU_COL_LO (SS_MENU_C0 - SS_MENU_BH + 1)   // 271
#define SS_MENU_COL_HI (SS_MENU_C0 + 1)                // 461

// Layout menu rect (top-left quadrant in landscape)
#define LM_R0     50
#define LM_RW    145
#define LM_C0    350
#define LM_BH    110
#define LM_COL_LO (LM_C0 - LM_BH + 1)   // 241
#define LM_COL_HI (LM_C0 + 1)            // 351

// Scale menu rect (same quadrant as layout menu — never open simultaneously)
#define SM_R0     50
#define SM_RW    200
#define SM_C0    350
#define SM_BH    140
#define SM_COL_LO (SM_C0 - SM_BH + 1)   // 211
#define SM_COL_HI (SM_C0 + 1)            // 351

// Scale / display mode
#define SCALE_FILL  0   // stretch to fill 800×480 (default)
#define SCALE_FIT   1   // aspect-correct 533×480, 133 px black bars left/right
#define SCALE_3X    2   // integer 3× → 480×432 centred with black border
#define SCALE_COUNT 3

static volatile int scale_mode         = SCALE_FILL;
static volatile int scale_menu_open    = 0;
static volatile int scale_cursor       = 0;
static volatile int scale_drawn_a      = 0;
static volatile int scale_drawn_b      = 0;
static volatile int scale_border_dirty = 0; // counts down; clears border on mode change
static inline void scale_invalidate(void) { scale_drawn_a = 0; scale_drawn_b = 0; }

static volatile int  ss_state      = SS_MENU_CLOSED;
static volatile int  ss_slot       = 0;
static volatile int  ss_cursor     = SS_SAVE;
static bool          ss_exists[10] = {false};
static char          ss_toast[32]  = {0};
static int           ss_toast_f    = 0;
static volatile int  ss_dirty      = 0;
static volatile int  ss_io_op      = 0;
static SemaphoreHandle_t sem_ss    = NULL;
static volatile int  ss_drawn_static = 0;
// Per-buffer flag: 1 = menu has been rendered into this buffer and game render skips it
static volatile int  ss_menu_drawn_a = 0;
static volatile int  ss_menu_drawn_b = 0;
static inline void ss_menu_invalidate(void) { ss_menu_drawn_a = 0; ss_menu_drawn_b = 0; }
static volatile int  key_layout        = 0;  // 0=default, 1=WASD
static volatile int  layout_menu_open  = 0;
static volatile int  lm_cursor         = 0;
static volatile int  lm_drawn_a        = 0;
static volatile int  lm_drawn_b        = 0;
static inline void lm_invalidate(void) { lm_drawn_a = 0; lm_drawn_b = 0; }



static volatile int  ff_speed = 0; // 0=1x 1=2x 2=3x

// Rewind: circular PSRAM snapshot buffer
#define REWIND_SLOTS      40
#define REWIND_STATE_SZ   (96*1024)
#define REWIND_SNAP_FREQ  15
static uint8_t  *rewind_state_buf   = NULL;
static uint16_t *rewind_pix_buf     = NULL;
static uint8_t  *rewind_sram_backup = NULL;  // protects in-game saves during rewind
static int       rw_sizes[REWIND_SLOTS] = {0};
static int       rw_head          = 0;
static int       rw_count         = 0;
static int       rw_pos           = 0;
static volatile int rewind_active = 0;
static int       rw_frame_ctr     = 0;
static uint16_t  rw_hold_pixels[GBC_WIDTH * GBC_HEIGHT];
static volatile int  return_to_selector = 0;
static volatile int  i2s_enabled = 1;
static volatile int  audio_mute = 0;
static SemaphoreHandle_t sem_audio_shutdown = NULL;
static volatile int  ss_clear_region  = 0; // counts down 2 frames to clear both bufs
static float gbc_volume = 100.0f;
static bool show_fps = false;
static int16_t *audio_buf_a   = NULL;
static int16_t *audio_buf_b   = NULL;
static volatile int audio_buf_ready = 0;
static volatile int audio_buf_len   = 0;
static SemaphoreHandle_t sem_audio_ready = NULL;
static SemaphoreHandle_t sem_audio_done  = NULL;
static float current_fps = 0.0f;
#define ROMS_DIR "/sdcard/roms"
#define MAX_ROMS 64
static char rom_list[MAX_ROMS][300];
static int  rom_count = 0;

static void scan_roms(void) {
    rom_count = 0;
    DIR *dir = opendir(ROMS_DIR);
    if (!dir) { ESP_LOGE("howboymatsu", "Cannot open roms dir"); return; }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && rom_count < MAX_ROMS) {
        char *name = entry->d_name;
        int len = strlen(name);
        if (len > 4) {
            char *ext = name + len - 4;
            if (strcasecmp(ext, ".gbc") == 0 || strcasecmp(ext, ".gb") == 0) {
                snprintf(rom_list[rom_count], sizeof(rom_list[0]), "%s/%s", ROMS_DIR, name);
                rom_count++;
            }
        }
    }
    closedir(dir);
    ESP_LOGI("howboymatsu", "Found %d ROMs", rom_count);
}

// Fast fill a PAX-rotated rectangle directly in pixel buffer
// PAX CW rotation: logical (x,y) -> physical [x*480 + (479-y)]
// For a full-width rect at logical y=row_y, h=row_h: fill cols (479-row_y) down to (479-row_y-row_h+1)
// Draw text directly into rotated PAX pixel buffer (bypasses slow PAX rotation)
// Draws at PAX logical coordinates (lx, ly) with given scale and RGB565 color
static const uint8_t rom_font5x7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // space
    {0x00,0x00,0x5F,0x00,0x00}, // !
    {0x00,0x07,0x00,0x07,0x00}, // "
    {0x14,0x7F,0x14,0x7F,0x14}, // #
    {0x24,0x2A,0x7F,0x2A,0x12}, // $
    {0x23,0x13,0x08,0x64,0x62}, // %
    {0x36,0x49,0x55,0x22,0x50}, // &
    {0x00,0x05,0x03,0x00,0x00}, // '
    {0x00,0x1C,0x22,0x41,0x00}, // (
    {0x00,0x41,0x22,0x1C,0x00}, // )
    {0x08,0x2A,0x1C,0x2A,0x08}, // *
    {0x08,0x08,0x3E,0x08,0x08}, // +
    {0x00,0x50,0x30,0x00,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08}, // -
    {0x00,0x60,0x60,0x00,0x00}, // .
    {0x20,0x10,0x08,0x04,0x02}, // /
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
    {0x00,0x36,0x36,0x00,0x00}, // :
    {0x00,0x56,0x36,0x00,0x00}, // ;
    {0x08,0x14,0x22,0x41,0x00}, // 
    {0x14,0x14,0x14,0x14,0x14}, // =
    {0x00,0x41,0x22,0x14,0x08}, // >
    {0x02,0x01,0x51,0x09,0x06}, // ?
    {0x32,0x49,0x79,0x41,0x3E}, // @
    {0x7E,0x11,0x11,0x11,0x7E}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x09,0x01}, // F
    {0x3E,0x41,0x49,0x49,0x7A}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x04,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7F,0x01,0x01}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x3F,0x40,0x38,0x40,0x3F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x07,0x08,0x70,0x08,0x07}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
    {0x00,0x7F,0x41,0x41,0x00}, // [
    {0x02,0x04,0x08,0x10,0x20}, // backslash
    {0x00,0x41,0x41,0x7F,0x00}, // ]
    {0x04,0x02,0x01,0x02,0x04}, // ^
    {0x40,0x40,0x40,0x40,0x40}, // _
    {0x00,0x01,0x02,0x04,0x00}, // `
    {0x20,0x54,0x54,0x54,0x78}, // a
    {0x7F,0x48,0x44,0x44,0x38}, // b
    {0x38,0x44,0x44,0x44,0x20}, // c
    {0x38,0x44,0x44,0x48,0x7F}, // d
    {0x38,0x54,0x54,0x54,0x18}, // e
    {0x08,0x7E,0x09,0x01,0x02}, // f
    {0x0C,0x52,0x52,0x52,0x3E}, // g
    {0x7F,0x08,0x04,0x04,0x78}, // h
    {0x00,0x44,0x7D,0x40,0x00}, // i
    {0x20,0x40,0x44,0x3D,0x00}, // j
    {0x7F,0x10,0x28,0x44,0x00}, // k
    {0x00,0x41,0x7F,0x40,0x00}, // l
    {0x7C,0x04,0x18,0x04,0x78}, // m
    {0x7C,0x08,0x04,0x04,0x78}, // n
    {0x38,0x44,0x44,0x44,0x38}, // o
    {0x7C,0x14,0x14,0x14,0x08}, // p
    {0x08,0x14,0x14,0x18,0x7C}, // q
    {0x7C,0x08,0x04,0x04,0x08}, // r
    {0x48,0x54,0x54,0x54,0x20}, // s
    {0x04,0x3F,0x44,0x40,0x20}, // t
    {0x3C,0x40,0x40,0x40,0x7C}, // u
    {0x1C,0x20,0x40,0x20,0x1C}, // v
    {0x3C,0x40,0x30,0x40,0x3C}, // w
    {0x44,0x28,0x10,0x28,0x44}, // x
    {0x0C,0x50,0x50,0x50,0x3C}, // y
    {0x44,0x64,0x54,0x4C,0x44}, // z
    {0x00,0x08,0x36,0x41,0x00}, // {
    {0x00,0x00,0x7F,0x00,0x00}, // |
    {0x00,0x41,0x36,0x08,0x00}, // }
    {0x08,0x08,0x2A,0x1C,0x08}, // ~
    {0x00,0x00,0x00,0x00,0x00}, // del
};

static void rom_draw_text_direct(const char *s, int lx, int ly, int sc, uint16_t color) {
    uint16_t *buf = (uint16_t *)pax_buf_get_pixels(&fb_pax);
    int cx = lx;
    for (int si = 0; s[si]; si++) {
        uint8_t c = (uint8_t)s[si];
        if (c < 32 || c > 127) { cx += 6 * sc; continue; }
        const uint8_t *glyph = rom_font5x7[c - 32];
        for (int fx = 0; fx < 5; fx++) {
            uint8_t col = glyph[fx];
            for (int fy = 0; fy < 7; fy++) {
                if (col & (1 << fy)) {
                    for (int sx = 0; sx < sc; sx++)
                        for (int sy = 0; sy < sc; sy++) {
                            int pr = (cx + fx * sc + sx);
                            int pc = 479 - (ly + fy * sc + sy);
                            if (pr >= 0 && pr < 800 && pc >= 0 && pc < 480)
                                buf[pr * 480 + pc] = color;
                        }
                }
            }
        }
        cx += 6 * sc;
    }
}

static void rom_fill_row_direct(int row_y, int row_h, uint16_t color565) {
    uint16_t *buf = (uint16_t *)pax_buf_get_pixels(&fb_pax);
    int col_start = 479 - row_y - row_h + 1;
    int col_end   = 479 - row_y;
    if (col_start < 0) col_start = 0;
    if (col_end > 479) col_end = 479;
    for (int r = 0; r < 800; r++) {
        for (int c = col_start; c <= col_end; c++)
            buf[r * 480 + c] = color565;
    }
}

static void rom_selector_draw_row(int idx, int scroll, int selected, int W, int LIST_Y, int ROW_H) {
    int i = idx - scroll;
    const char *fname = strrchr(rom_list[idx], '/');
    fname = fname ? fname + 1 : rom_list[idx];
    char display[120];
    strncpy(display, fname, sizeof(display)-1);
    display[sizeof(display)-1] = 0;
    char *dot = strrchr(display, '.');
    if (dot) *dot = 0;
    int row_y = LIST_Y + i * ROW_H;
    // Background already filled by rom_fill_row_direct — draw text directly
    if (idx == selected) {
        rom_draw_text_direct(display, 12, row_y + 6, 2, 0x0000); // black on green
    } else {
        rom_draw_text_direct(display, 12, row_y + 6, 2, 0xDEDB); // light gray
    }
}

static const char *rom_selector(void) {
    if (rom_count == 0) return NULL;
    int selected = 0, scroll = 0;
    const int W = 800, H = 480;
    const int HEADER_H = 60;
    const int FOOTER_H = 36;
    const int LIST_Y = 68;
    const int ROW_H = 32;
    const int visible = (H - HEADER_H - FOOTER_H - 16) / ROW_H;
    bool full_redraw = true;
    int prev_selected = -1;
    int prev_scroll = -1;
    while (1) {
        bool scroll_changed = (scroll != prev_scroll);
        if (full_redraw || scroll_changed) {
            pax_background(&fb_pax, 0xFF111111);
            pax_simple_rect(&fb_pax, 0xFF2E1A1A, 0, 0, W, HEADER_H);
            pax_draw_text(&fb_pax, 0xFFFF0000, pax_font_sky_mono, 28, 16, 14, "HowBoyMatsu");
            char counter[32];
            snprintf(counter, sizeof(counter), "%d/%d", selected + 1, rom_count);
            pax_draw_text(&fb_pax, 0xFF888888, pax_font_sky_mono, 18, W - 100, 20, counter);
            pax_simple_rect(&fb_pax, 0xFFFF0000, 0, HEADER_H, W, 2);
            // Fast direct fill for all row backgrounds
            for (int i = 0; i < visible && (scroll + i) < rom_count; i++) {
                int idx = scroll + i;
                int row_y = LIST_Y + i * ROW_H;
                uint16_t bg565;
                if (idx == selected)       bg565 = 0x8000;
                else if (i % 2 == 0)       bg565 = 0x1800;
                else                       bg565 = 0x2000;
                rom_fill_row_direct(row_y, ROW_H, bg565);
            }
            for (int i = 0; i < visible && (scroll + i) < rom_count; i++)
                rom_selector_draw_row(scroll + i, scroll, selected, W, LIST_Y, ROW_H);
            pax_simple_rect(&fb_pax, 0xFF2E1A1A, 0, H - FOOTER_H, W, FOOTER_H);
            pax_simple_rect(&fb_pax, 0xFFFF0000, 0, H - FOOTER_H, W, 2);
            pax_draw_text(&fb_pax, 0xFF888888, pax_font_sky_mono, 14, 12, H - FOOTER_H + 10,
                          "[Up/Down] Navigate   [Enter/A] Launch   [ESC] Exit");
            full_redraw = false;
        } else if (prev_selected != selected) {
            // Only redraw the two changed rows + counter
            pax_simple_rect(&fb_pax, 0xFF2E1A1A, W - 110, 10, 110, 30);
            char counter[32];
            snprintf(counter, sizeof(counter), "%d/%d", selected + 1, rom_count);
            pax_draw_text(&fb_pax, 0xFF888888, pax_font_sky_mono, 18, W - 100, 20, counter);
            if (prev_selected >= scroll && prev_selected < scroll + visible) {
                int pi = prev_selected - scroll;
                uint16_t bg = (pi % 2 == 0) ? 0x1800 : 0x2000;
                rom_fill_row_direct(LIST_Y + pi * ROW_H, ROW_H, bg);
                rom_selector_draw_row(prev_selected, scroll, selected, W, LIST_Y, ROW_H);
            }
            if (selected >= scroll && selected < scroll + visible) {
                rom_fill_row_direct(LIST_Y + (selected - scroll) * ROW_H, ROW_H, 0x8000);
                rom_selector_draw_row(selected, scroll, selected, W, LIST_Y, ROW_H);
            }
        }
        prev_selected = selected;
        prev_scroll = scroll;
        blit();
        bsp_input_event_t ev;
        if (xQueueReceive(input_event_queue, &ev, portMAX_DELAY) == pdTRUE) {
            if (ev.type == INPUT_EVENT_TYPE_NAVIGATION && ev.args_navigation.state == 1) {
                switch (ev.args_navigation.key) {
                    case BSP_INPUT_NAVIGATION_KEY_UP:
                        if (selected > 0) {
                            selected--;
                            if (selected < scroll) scroll = selected;
                        }
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_DOWN:
                        if (selected < rom_count - 1) {
                            selected++;
                            if (selected >= scroll + visible) scroll = selected - visible + 1;
                        }
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_RETURN:
                        return rom_list[selected];
                    case BSP_INPUT_NAVIGATION_KEY_ESC:
                        if (blit_task_handle) vTaskSuspend(blit_task_handle);
                        vTaskDelay(pdMS_TO_TICKS(20));
                        if (render_buf_a) {
                            memset(render_buf_a, 0, 480 * 800 * 2);
                            bsp_display_blit(0, 0, 480, 800, render_buf_a);
                            vTaskDelay(pdMS_TO_TICKS(50));
                            bsp_display_blit(0, 0, 480, 800, render_buf_a);
                        }
                        bsp_audio_set_volume(0);
                        vTaskDelay(pdMS_TO_TICKS(150));
                        restart_to_launcher();
                        break;
                    default: break;
                }
            } else if (ev.type == INPUT_EVENT_TYPE_KEYBOARD && ev.args_keyboard.ascii == 'a') {
                return rom_list[selected];
            }
        }
    }
}

void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb_pax));
}
// Draw the GBC screen scaled up onto the Tanmatsu display
static uint16_t scaled_row_565[480];


void draw_gbc_screen(void) {
    const int GBC_W  = 160;
    const int GBC_H  = 144;
    const int PHYS_W = 480;
    const int V_SCALE = 5;

    xSemaphoreTake(sem_frame_done, portMAX_DELAY);
    uint16_t *phys = (uint16_t *)((active_render_buf == 0) ? render_buf_b : render_buf_a);

    static int init_done = 0;
    if (!init_done) {
        memset(render_buf_a, 0, PHYS_W * 800 * 2);
        memset(render_buf_b, 0, PHYS_W * 800 * 2);
        init_done = 1;
    }

    // Which menus are open (their rects must be preserved across frames)
    int menu_open = (ss_state == SS_MENU_OPEN || ss_state == SS_MENU_SAVING || ss_state == SS_MENU_LOADING);
    int lm_open   = layout_menu_open;
    int sm_open   = scale_menu_open;

    // On scale mode change: zero the black border for 2 frames (both render buffers).
    // FILL overwrites every row each frame so needs no clearing.
    if (scale_border_dirty > 0) {
        if (scale_mode == SCALE_FIT) {
            memset(phys,               0, 133 * PHYS_W * 2);  // rows 0..132
            memset(phys + 666 * PHYS_W, 0, 134 * PHYS_W * 2); // rows 666..799
        } else if (scale_mode == SCALE_3X) {
            memset(phys,               0, 160 * PHYS_W * 2);  // rows 0..159
            memset(phys + 640 * PHYS_W, 0, 160 * PHYS_W * 2); // rows 640..799
        }
        scale_border_dirty--;
    }

    if (scale_mode == SCALE_FILL) {
        // ── FILL: stretch 160×144 to 800×480 (5× H, 3.33× V) ──────────────
        for (int gx = 0; gx < GBC_W; gx++) {
            uint16_t *rp = scaled_row_565;
            for (int gy = GBC_H - 1; gy >= 0; gy--) {
                uint16_t pixel = gbc_pixels[gy * GBC_W + gx];
                uint8_t r5 = (pixel >> 11) & 0x1F;
                uint8_t g6 = (pixel >> 5)  & 0x3F;
                uint8_t b5 =  pixel        & 0x1F;
                uint16_t p565 = (r5 << 11) | (g6 << 5) | b5;
                *rp++ = p565; *rp++ = p565; *rp++ = p565;
                if (gy % 3 == 0) { *rp++ = p565; }
            }
            int row_start = gx * V_SCALE;
            for (int rep = 0; rep < V_SCALE; rep++) {
                int row = row_start + rep;
                uint16_t *dst = phys + row * PHYS_W;
                if (menu_open && row >= SS_MENU_R0 && row < SS_MENU_R0 + SS_MENU_RW) {
                    if (SS_MENU_COL_LO > 0)
                        memcpy(dst, scaled_row_565, SS_MENU_COL_LO * 2);
                    if (SS_MENU_COL_HI < PHYS_W)
                        memcpy(dst + SS_MENU_COL_HI, scaled_row_565 + SS_MENU_COL_HI, (PHYS_W - SS_MENU_COL_HI) * 2);
                } else if (lm_open && row >= LM_R0 && row < LM_R0 + LM_RW) {
                    if (LM_COL_LO > 0)
                        memcpy(dst, scaled_row_565, LM_COL_LO * 2);
                    if (LM_COL_HI < PHYS_W)
                        memcpy(dst + LM_COL_HI, scaled_row_565 + LM_COL_HI, (PHYS_W - LM_COL_HI) * 2);
                } else if (sm_open && row >= SM_R0 && row < SM_R0 + SM_RW) {
                    if (SM_COL_LO > 0)
                        memcpy(dst, scaled_row_565, SM_COL_LO * 2);
                    if (SM_COL_HI < PHYS_W)
                        memcpy(dst + SM_COL_HI, scaled_row_565 + SM_COL_HI, (PHYS_W - SM_COL_HI) * 2);
                } else {
                    memcpy(dst, scaled_row_565, PHYS_W * 2);
                }
            }
        }
    } else if (scale_mode == SCALE_FIT) {
        // ── FIT: aspect-correct 533×480, 133 px black bars left/right ──────
        // Both axes scale at 480/144 = 3.33×.
        // GBC X: 160 cols → 533 rows starting at row 133.
        //   row_count = 3 for most gx; 4 when gx%3==0 && gx>0 (53 such cols → 533 total).
        int row = 133;
        for (int gx = 0; gx < GBC_W; gx++) {
            uint16_t *rp = scaled_row_565;
            for (int gy = GBC_H - 1; gy >= 0; gy--) {
                uint16_t pixel = gbc_pixels[gy * GBC_W + gx];
                uint8_t r5 = (pixel >> 11) & 0x1F;
                uint8_t g6 = (pixel >> 5)  & 0x3F;
                uint8_t b5 =  pixel        & 0x1F;
                uint16_t p565 = (r5 << 11) | (g6 << 5) | b5;
                *rp++ = p565; *rp++ = p565; *rp++ = p565;
                if (gy % 3 == 0) { *rp++ = p565; }
            }
            int row_count = (gx > 0 && gx % 3 == 0) ? 4 : 3;
            for (int rep = 0; rep < row_count; rep++, row++) {
                uint16_t *dst = phys + row * PHYS_W;
                if (menu_open && row >= SS_MENU_R0 && row < SS_MENU_R0 + SS_MENU_RW) {
                    if (SS_MENU_COL_LO > 0)
                        memcpy(dst, scaled_row_565, SS_MENU_COL_LO * 2);
                    if (SS_MENU_COL_HI < PHYS_W)
                        memcpy(dst + SS_MENU_COL_HI, scaled_row_565 + SS_MENU_COL_HI, (PHYS_W - SS_MENU_COL_HI) * 2);
                } else if (lm_open && row >= LM_R0 && row < LM_R0 + LM_RW) {
                    if (LM_COL_LO > 0)
                        memcpy(dst, scaled_row_565, LM_COL_LO * 2);
                    if (LM_COL_HI < PHYS_W)
                        memcpy(dst + LM_COL_HI, scaled_row_565 + LM_COL_HI, (PHYS_W - LM_COL_HI) * 2);
                } else if (sm_open && row >= SM_R0 && row < SM_R0 + SM_RW) {
                    if (SM_COL_LO > 0)
                        memcpy(dst, scaled_row_565, SM_COL_LO * 2);
                    if (SM_COL_HI < PHYS_W)
                        memcpy(dst + SM_COL_HI, scaled_row_565 + SM_COL_HI, (PHYS_W - SM_COL_HI) * 2);
                } else {
                    memcpy(dst, scaled_row_565, PHYS_W * 2);
                }
            }
        }
    } else {
        // ── 3×: integer 3× scale → 480×432, centred in 800×480 ─────────────
        // GBC X (0..159) → rows 160..639 (3 rows each, exact).
        // GBC Y (0..143) → cols 24..455 (3 cols each, exact).
        // Border positions in scaled_row_565 are zeroed once before the loop.
        memset(scaled_row_565,       0, 24 * sizeof(uint16_t)); // cols 0..23
        memset(scaled_row_565 + 456, 0, 24 * sizeof(uint16_t)); // cols 456..479
        for (int gx = 0; gx < GBC_W; gx++) {
            uint16_t *rp = scaled_row_565 + 24;
            for (int gy = GBC_H - 1; gy >= 0; gy--) {
                uint16_t pixel = gbc_pixels[gy * GBC_W + gx];
                uint8_t r5 = (pixel >> 11) & 0x1F;
                uint8_t g6 = (pixel >> 5)  & 0x3F;
                uint8_t b5 =  pixel        & 0x1F;
                uint16_t p565 = (r5 << 11) | (g6 << 5) | b5;
                *rp++ = p565; *rp++ = p565; *rp++ = p565;
            }
            int row_start = gx * 3 + 160;
            for (int rep = 0; rep < 3; rep++) {
                int row = row_start + rep;
                uint16_t *dst = phys + row * PHYS_W;
                if (menu_open && row >= SS_MENU_R0 && row < SS_MENU_R0 + SS_MENU_RW) {
                    if (SS_MENU_COL_LO > 0)
                        memcpy(dst, scaled_row_565, SS_MENU_COL_LO * 2);
                    if (SS_MENU_COL_HI < PHYS_W)
                        memcpy(dst + SS_MENU_COL_HI, scaled_row_565 + SS_MENU_COL_HI, (PHYS_W - SS_MENU_COL_HI) * 2);
                } else if (lm_open && row >= LM_R0 && row < LM_R0 + LM_RW) {
                    if (LM_COL_LO > 0)
                        memcpy(dst, scaled_row_565, LM_COL_LO * 2);
                    if (LM_COL_HI < PHYS_W)
                        memcpy(dst + LM_COL_HI, scaled_row_565 + LM_COL_HI, (PHYS_W - LM_COL_HI) * 2);
                } else if (sm_open && row >= SM_R0 && row < SM_R0 + SM_RW) {
                    if (SM_COL_LO > 0)
                        memcpy(dst, scaled_row_565, SM_COL_LO * 2);
                    if (SM_COL_HI < PHYS_W)
                        memcpy(dst + SM_COL_HI, scaled_row_565 + SM_COL_HI, (PHYS_W - SM_COL_HI) * 2);
                } else {
                    memcpy(dst, scaled_row_565, PHYS_W * 2);
                }
            }
        }
    }

    if (ss_clear_region > 0) ss_clear_region--;
    active_render_buf ^= 1;
    xSemaphoreGive(sem_frame_ready);
}
// --- gnuboy platform callbacks ---
void vid_preinit(void) {}

void vid_init(void) {
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
    ESP_LOGI(TAG, "PAX buf ptr: %p  w=%d h=%d", pax_buf_get_pixels(&fb_pax), pax_buf_get_width(&fb_pax), pax_buf_get_height(&fb_pax));
}
void vid_close(void) {}

// Release every Game Boy button after rewind exits so restored
// joypad state doesn't keep the character moving.
static void rewind_release_all_keys(void) {
    pad_set(PAD_UP,     0);
    pad_set(PAD_DOWN,   0);
    pad_set(PAD_LEFT,   0);
    pad_set(PAD_RIGHT,  0);
    pad_set(PAD_A,      0);
    pad_set(PAD_B,      0);
    pad_set(PAD_SELECT, 0);
    pad_set(PAD_START,  0);
}

static void rewind_push(void) {
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

static int rewind_pop(void) {
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

void vid_begin(void) {
    frame++;
    fb.dirty = 1;
    pal_dirty();
}

void vid_end(void) {
    static int frame_count = 0;
    static int64_t last_time = 0;
    frame_count++;
    if (last_time == 0) last_time = esp_timer_get_time();
    if (return_to_selector) {
        bsp_audio_set_volume(0);
        if (blit_task_handle) vTaskSuspend(blit_task_handle);
        if (render_buf_a) memset(render_buf_a, 0, 480 * 800 * 2);
        if (render_buf_b) memset(render_buf_b, 0, 480 * 800 * 2);
        bsp_display_blit(0, 0, 480, 800, render_buf_a);
        vTaskDelay(pdMS_TO_TICKS(30));
        bsp_display_blit(0, 0, 480, 800, render_buf_a);
        // Disable I2S if still enabled
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
                // Exhausted all history — trim buffer and stop
                rw_head  = ((rw_head - rw_pos) % REWIND_SLOTS + REWIND_SLOTS) % REWIND_SLOTS;
                rw_count = 0;
                rw_pos        = 0;
                rewind_active = 0;
                audio_mute    = 0;
                rewind_release_all_keys();
                // Restore SRAM from pre-rewind backup to protect in-game saves
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
    // Fast forward: ff_speed 0=1x, 1=3x(skip 2), 2=5x(skip 4)
    static int ff_frame = 0;
    static const int ff_skip[] = {0, 4, 7}; // 1x, 5x, 8x
    int skip = ff_skip[ff_speed];
    if (skip > 0) {
        ff_frame++;
        if (ff_frame <= skip) {
            fb.dirty = 0; // skip render this frame
        } else {
            ff_frame = 0;
            if (fb.dirty) { draw_gbc_screen(); fb.dirty = 0; }
        }
    } else {
        ff_frame = 0;
        if (fb.dirty) { draw_gbc_screen(); fb.dirty = 0; }
    }
    // Snapshot for rewind (only during normal 1x playback, not while rewinding)
    if (ff_speed == 0 && !rewind_active) {
        static int rw_snap_ctr = 0;
        if (++rw_snap_ctr >= REWIND_SNAP_FREQ) {
            rw_snap_ctr = 0;
            rewind_push();
        }
    }
    // Autosave SRAM every hour (~216000 frames at 60fps)
    if (frame_count % 18000 == 0 && frame_count > 0 && sram_path_global[0]) {
        FILE *sf = fopen(sram_path_global, "wb");
        if (sf) { sram_save(sf); fclose(sf); ESP_LOGI("howboymatsu", "SRAM autosaved"); }
        char rtc_path3[320];
        strncpy(rtc_path3, sram_path_global, sizeof(rtc_path3)-1);
        char *rdot3 = strrchr(rtc_path3, '.');
        if (rdot3) strcpy(rdot3, ".rtc");
        FILE *rf3 = fopen(rtc_path3, "wb");
        if (rf3) { rtc_save(rf3); }  // rtc_save closes file internally
    }
    if (frame_count % 10 == 0) {
        int64_t now = esp_timer_get_time();
        float fps = 10.0f / ((now - last_time) / 1000000.0f);
        current_fps = fps;
        ESP_LOGI(TAG, "FPS: %.1f", fps);
        // Refresh palette for first 5 seconds after SRAM load to fix green hue
        if (frame_count < 300 && sram_path_global[0]) {
            pal_dirty();
            vram_dirty();
        }

        last_time = now;
    }
}
void vid_setpal(int i, int r, int g, int b) {
    (void)i; (void)r; (void)g; (void)b;
}

void vid_settitle(char *title) {
    ESP_LOGI(TAG, "Game title: %s", title);
}

// --- gnuboy audio callbacks (stub for now) ---
void pcm_init(void) {
    static int inited = 0;
    if (inited) {
        if (pcm.buf)     memset(pcm.buf,     0, pcm.len * sizeof(int16_t));
        if (audio_buf_a) memset(audio_buf_a, 0, pcm.len * sizeof(int16_t));
        if (audio_buf_b) memset(audio_buf_b, 0, pcm.len * sizeof(int16_t));
        pcm.pos = 0;
        ESP_LOGI(TAG, "Audio reset for new ROM");
        return;
    }
    inited = 1;

    pcm.hz     = 44100;
    pcm.stereo = 1;
    pcm.len    = (44100 / 60) * 2;  // 1 frame of stereo samples at 44100Hz
    pcm.buf    = (int16_t *)malloc(pcm.len * sizeof(int16_t));
    audio_buf_a = (int16_t *)malloc(pcm.len * sizeof(int16_t));
    audio_buf_b = (int16_t *)malloc(pcm.len * sizeof(int16_t));
    sem_audio_ready = xSemaphoreCreateBinary();
    sem_audio_done  = xSemaphoreCreateBinary();
    xSemaphoreGive(sem_audio_done);
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 6, &audio_task_handle, 0);
    pcm.pos    = 0;
    memset(pcm.buf, 0, pcm.len * sizeof(int16_t));
    ESP_LOGI(TAG, "Audio initialized at %dHz stereo len=%d", pcm.hz, pcm.len);
}

int pcm_submit(void) {
    if (audio_mute) { pcm.pos = 0; return 1; }
    if (!pcm.buf || pcm.pos == 0) { pcm.pos = 0; return 1; }
    if (!sem_audio_ready || !sem_audio_done) { pcm.pos = 0; return 1; }
    // Fast forward: feed silence non-blocking so I2S DMA stays quiet.
    if (ff_speed > 0) {
        if (xSemaphoreTake(sem_audio_done, 0) == pdTRUE) {
            int16_t *ready = (audio_buf_ready == 0) ? audio_buf_a : audio_buf_b;
            memset(ready, 0, pcm.len * sizeof(int16_t));
            audio_buf_len = pcm.len;
            audio_buf_ready ^= 1;
            xSemaphoreGive(sem_audio_ready);
        }
        pcm.pos = 0;
        return 1;
    }
    xSemaphoreTake(sem_audio_done, pdMS_TO_TICKS(30));
    int16_t *ready = (audio_buf_ready == 0) ? audio_buf_a : audio_buf_b;
    memcpy(ready, pcm.buf, pcm.pos * sizeof(int16_t));
    audio_buf_len = pcm.pos;
    audio_buf_ready ^= 1;
    pcm.pos = 0;
    xSemaphoreGive(sem_audio_ready);
    return 1;
}

void pcm_close(void) {
    if (pcm.buf) free(pcm.buf);
}

// --- gnuboy system callbacks ---
void sys_sleep(int us) {
    (void)us;
}

void *sys_timer(void) { return NULL; }
int   sys_elapsed(void *ptr) { (void)ptr; return 0; }
void sys_checkdir(char *path, int wr) { (void)path; (void)wr; }
void sys_sanitize(char *s)            { (void)s; }
void sys_initpath(char *exe)          { (void)exe; }

// --- gnuboy input ---
static int gbc_keys[8] = {0};
void doevents(void) {
    bsp_input_event_t event;
    // Indices 0-3: A, B, Start, Select (both layouts)
    // Indices 4-7: Up, Down, Left, Right (WASD layout keyboard d-pad)
    static uint32_t key_release_time[8] = {0};
    static byte key_pads[8] = {PAD_A, PAD_B, PAD_START, PAD_SELECT, PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT};

    // Auto-release keys after 100ms
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

            // Save state menu input routing
            if (ss_state == SS_MENU_OPEN && pressed) {
                switch (event.args_navigation.key) {
                    case BSP_INPUT_NAVIGATION_KEY_UP:
                        ss_cursor--; if (ss_cursor < SS_SAVE) ss_cursor = SS_CANCEL;
                        // skip LOAD and DELETE when slot is empty
                        if ((ss_cursor == SS_LOAD || ss_cursor == SS_DELETE) && !ss_exists[ss_slot]) ss_cursor = SS_SAVE;
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_DOWN:
                        ss_cursor++; if (ss_cursor > SS_CANCEL) ss_cursor = SS_SAVE;
                        // skip LOAD and DELETE when slot is empty
                        if ((ss_cursor == SS_LOAD || ss_cursor == SS_DELETE) && !ss_exists[ss_slot]) ss_cursor = SS_CANCEL;
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_LEFT:
                        ss_slot--; if (ss_slot < 0) ss_slot = 9;
                        if ((ss_cursor == SS_LOAD || ss_cursor == SS_DELETE) && !ss_exists[ss_slot]) ss_cursor = SS_SAVE;
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_RIGHT:
                        ss_slot++; if (ss_slot > 9) ss_slot = 0;
                        if ((ss_cursor == SS_LOAD || ss_cursor == SS_DELETE) && !ss_exists[ss_slot]) ss_cursor = SS_SAVE;
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_RETURN:
                        if (ss_cursor == SS_CANCEL) {
                            ss_state = SS_MENU_CLOSED;
                        } else if (ss_cursor == SS_SAVE && ss_io_op == 0) {
                            ss_io_op = 1; ss_state = SS_MENU_SAVING;
                            xSemaphoreGive(sem_ss);
                        } else if (ss_cursor == SS_LOAD && ss_exists[ss_slot] && ss_io_op == 0) {
                            ss_io_op = 2; ss_state = SS_MENU_LOADING;
                            xSemaphoreGive(sem_ss);
                        } else if (ss_cursor == SS_DELETE && ss_exists[ss_slot] && ss_io_op == 0) {
                            ss_io_op = 3; ss_state = SS_MENU_SAVING;
                            xSemaphoreGive(sem_ss);
                        }
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_F6:
                    if (pressed) {
                        ff_speed = (ff_speed + 1) % 3;
                        const char *ff_labels[] = {"OFF", "5x", "8x"};
                        ESP_LOGI(TAG, "Fast forward: %s", ff_labels[ff_speed]);
                    }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_F4:
                        ss_state = SS_MENU_CLOSED; break;
                    default: break;
                }
                ss_menu_invalidate(); // any handled key may have changed cursor/slot/state
                continue; // swallow all input while menu open
            }

            // Scale menu input routing
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
                        scale_mode = scale_cursor;
                        scale_menu_open = 0;
                        scale_border_dirty = 2;
                        scale_invalidate();
                        ESP_LOGI(TAG, "Scale: %s", (const char*[]){"STRETCH","FIT","3X"}[scale_mode]);
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_F3:
                        scale_menu_open = 0; scale_invalidate(); break;
                    default: break;
                }
                continue;
            }

            // Layout menu input routing
            if (layout_menu_open && pressed) {
                switch (event.args_navigation.key) {
                    case BSP_INPUT_NAVIGATION_KEY_UP:
                        lm_cursor = 0; lm_invalidate(); break;
                    case BSP_INPUT_NAVIGATION_KEY_DOWN:
                        lm_cursor = 1; lm_invalidate(); break;
                    case BSP_INPUT_NAVIGATION_KEY_RETURN:
                        key_layout = lm_cursor;
                        layout_menu_open = 0;
                        lm_invalidate();
                        pad_set(PAD_UP, 0); pad_set(PAD_DOWN, 0);
                        pad_set(PAD_LEFT, 0); pad_set(PAD_RIGHT, 0);
                        key_release_time[4] = key_release_time[5] = key_release_time[6] = key_release_time[7] = 0;
                        ESP_LOGI(TAG, "Layout: %s", key_layout ? "WASD" : "Default");
                        break;
                    case BSP_INPUT_NAVIGATION_KEY_F2:
                        layout_menu_open = 0; lm_invalidate(); break;
                    default: break;
                }
                continue;
            }

            switch (event.args_navigation.key) {
                case BSP_INPUT_NAVIGATION_KEY_UP:    if (key_layout == 0) pad_set(PAD_UP,    pressed); break;
                case BSP_INPUT_NAVIGATION_KEY_DOWN:  if (key_layout == 0) pad_set(PAD_DOWN,  pressed); break;
                case BSP_INPUT_NAVIGATION_KEY_LEFT:  if (key_layout == 0) pad_set(PAD_LEFT,  pressed); break;
                case BSP_INPUT_NAVIGATION_KEY_RIGHT: if (key_layout == 0) pad_set(PAD_RIGHT, pressed); break;
                case BSP_INPUT_NAVIGATION_KEY_RETURN: pad_set(PAD_START,  pressed); break;
                case BSP_INPUT_NAVIGATION_KEY_ESC:
                    if (pressed) {
                        if (sram_path_global[0]) {
                            FILE *sf = fopen(sram_path_global, "wb");
                            if (sf) { sram_save(sf); fclose(sf); ESP_LOGI("howboymatsu", "SRAM saved on exit"); }
                            char rtc_path2[320];
                            strncpy(rtc_path2, sram_path_global, sizeof(rtc_path2)-1);
                            char *rdot2 = strrchr(rtc_path2, '.');
                            if (rdot2) strcpy(rdot2, ".rtc");
                            FILE *rf = fopen(rtc_path2, "wb");
                            if (rf) { rtc_save(rf); }  // rtc_save closes file internally
                        }
                        // Clean exit: stop blit task, black screen, stop audio
                        if (blit_task_handle) vTaskSuspend(blit_task_handle);
                        vTaskDelay(pdMS_TO_TICKS(20));
                        if (render_buf_a) {
                            memset(render_buf_a, 0, 480 * 800 * 2);
                            bsp_display_blit(0, 0, 480, 800, render_buf_a);
                            vTaskDelay(pdMS_TO_TICKS(50));
                            bsp_display_blit(0, 0, 480, 800, render_buf_a);
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
                        ESP_LOGI(TAG, "Volume: %.0f%%", gbc_volume);
                    }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN:
                    if (pressed) {
                        gbc_volume -= 5.0f;
                        if (gbc_volume < 0.0f) gbc_volume = 0.0f;
                        bsp_audio_set_volume(gbc_volume);
                        ESP_LOGI(TAG, "Volume: %.0f%%", gbc_volume);
                    }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_F6:
                    if (pressed) {
                        ff_speed = (ff_speed + 1) % 3;
                        const char *ff_labels[] = {"OFF", "5x", "8x"};
                        ESP_LOGI(TAG, "Fast forward: %s", ff_labels[ff_speed]);
                    }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_F3:
                    if (pressed && ss_state == SS_MENU_CLOSED && !layout_menu_open) {
                        scale_cursor = scale_mode;
                        scale_menu_open = 1;
                        scale_invalidate();
                    }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_F2:
                    if (pressed && ss_state == SS_MENU_CLOSED) {
                        lm_cursor = key_layout;
                        layout_menu_open = 1;
                        lm_invalidate();
                    }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_F1:
                    if (pressed && ss_state == SS_MENU_CLOSED && !layout_menu_open) {
                        memset(gbc_pixels, 0, sizeof(gbc_pixels));
                        emu_reset();
                        vram_dirty();
                        pal_dirty();
                        sound_dirty();
                        mem_updatemap();
                        pcm_init();
                        ESP_LOGI(TAG, "Soft reset");
                    }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_F5:
                    if (pressed && ss_state == SS_MENU_CLOSED && !layout_menu_open) {
                        if (!rewind_active) {
                            if (rewind_state_buf && rw_count > 0) {
                                // Save SRAM before rewinding to protect in-game saves
                                if (rewind_sram_backup && ram.sbank && mbc.ramsize > 0) {
                                    memcpy(rewind_sram_backup, ram.sbank, (size_t)mbc.ramsize * 8192);
                                }
                                rewind_active = 1;
                                audio_mute    = 1;
                                rw_pos        = 0;
                                rw_frame_ctr  = 2;  // forces pop on the very first vid_end frame
                                ESP_LOGI(TAG, "Rewind ON (%d snapshots)", rw_count);
                            }
                        } else {
                            // Exit rewind: trim buffer to current resume point
                            rw_head  = ((rw_head - rw_pos) % REWIND_SLOTS + REWIND_SLOTS) % REWIND_SLOTS;
                            rw_count = (rw_count > rw_pos) ? rw_count - rw_pos : 0;
                            rw_pos        = 0;
                            rewind_active = 0;
                            audio_mute    = 0;
                            rewind_release_all_keys();
                            // Restore SRAM from backup to protect in-game saves
                            if (rewind_sram_backup && ram.sbank && mbc.ramsize > 0) {
                                memcpy(ram.sbank, rewind_sram_backup, (size_t)mbc.ramsize * 8192);
                                ram.sram_dirty = 1;
                            }
                            ESP_LOGI(TAG, "Rewind OFF");
                        }
                    }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_F4:
                    if (pressed && ss_state == SS_MENU_CLOSED && !layout_menu_open && state_save_dir[0]) {
                        for (int si = 0; si < 10; si++) {
                            char spath[340];
                            snprintf(spath, sizeof(spath), "%s.ss%d", state_save_dir, si);
                            struct stat st;
                            ss_exists[si] = (stat(spath, &st) == 0);
                        }
                        ss_cursor = SS_SAVE;
                        ss_drawn_static = 0;
                        ss_clear_region  = 0;
                        ss_state = SS_MENU_OPEN;
                        ss_menu_invalidate();
                    }
                    break;
                default: break;
            }
        } else if (event.type == INPUT_EVENT_TYPE_KEYBOARD) {
            // Backtick always toggles FPS regardless of menu state
            if (event.args_keyboard.ascii == '`') {
                show_fps = !show_fps;
                if (!show_fps) {
                    // Clear FPS region in both render buffers
                    uint16_t *pa = (uint16_t *)render_buf_a;
                    uint16_t *pb = (uint16_t *)render_buf_b;
                    for (int r = 2; r < 13; r++)
                        for (int c = 2; c < 58; c++) {
                            pa[r * 480 + c] = 0x0000;
                            pb[r * 480 + c] = 0x0000;
                        }
                }
                continue;
            }
            // Layout menu: 'a' confirms selection, all other keys swallowed
            if (layout_menu_open) {
                if (event.args_keyboard.ascii == 'a' || event.args_keyboard.ascii == 'A') {
                    key_layout = lm_cursor;
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
                // Default layout: a=GBC-A, d=GBC-B
                switch (event.args_keyboard.ascii) {
                    case 'a': case 'A':
                        pad_set(PAD_A, 1); key_release_time[0] = release_at; break;
                    case 'd': case 'D':
                        pad_set(PAD_B, 1); key_release_time[1] = release_at; break;
                    case '\n': case '\r':
                        pad_set(PAD_START, 1); key_release_time[2] = release_at; break;
                    case ' ':
                        pad_set(PAD_SELECT, 1); key_release_time[3] = release_at; break;
                    case '\b': case 127:
                        if (sram_path_global[0]) {
                            FILE *sf = fopen(sram_path_global, "wb");
                            if (sf) { sram_save(sf); fclose(sf); ESP_LOGI(TAG, "SRAM saved on ROM selector return"); }
                            char rtc_path_bs[320];
                            strncpy(rtc_path_bs, sram_path_global, sizeof(rtc_path_bs)-1);
                            char *rdot_bs = strrchr(rtc_path_bs, '.');
                            if (rdot_bs) strcpy(rdot_bs, ".rtc");
                            FILE *rf_bs = fopen(rtc_path_bs, "wb");
                            if (rf_bs) { rtc_save(rf_bs); }
                        }
                        bsp_audio_set_amplifier(false);
                        bsp_audio_set_volume(0);
                        audio_mute = 1;
                        return_to_selector = 1;
                        break;
                    default: break;
                }
            } else {
                // WASD layout: w/a/s/d=d-pad, ;=GBC-A, [=GBC-B
                switch (event.args_keyboard.ascii) {
                    // w/a/s/d, ;, [ handled by scancode polling above
                    case '\n': case '\r':
                        pad_set(PAD_START,  1); key_release_time[2] = release_at; break;
                    case ' ':
                        pad_set(PAD_SELECT, 1); key_release_time[3] = release_at; break;
                    case '\b': case 127:
                        if (sram_path_global[0]) {
                            FILE *sf = fopen(sram_path_global, "wb");
                            if (sf) { sram_save(sf); fclose(sf); ESP_LOGI(TAG, "SRAM saved on ROM selector return"); }
                            char rtc_path_bs[320];
                            strncpy(rtc_path_bs, sram_path_global, sizeof(rtc_path_bs)-1);
                            char *rdot_bs = strrchr(rtc_path_bs, '.');
                            if (rdot_bs) strcpy(rdot_bs, ".rtc");
                            FILE *rf_bs = fopen(rtc_path_bs, "wb");
                            if (rf_bs) { rtc_save(rf_bs); }
                        }
                        bsp_audio_set_amplifier(false);
                        bsp_audio_set_volume(0);
                        audio_mute = 1;
                        return_to_selector = 1;
                        break;
                    default: break;
                }
            }
        }
    }
}

void ev_poll(void) {
    doevents();
}

// gnuboy die() handler
void die(char *fmt, ...) {
    va_list ap;
    char diebuf[256];
    va_start(ap, fmt);
    vsnprintf(diebuf, sizeof(diebuf), fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "%s", diebuf);
    abort();
}


// ── Save state 5x7 font (ASCII 32-122) ──────────────────────────────────────
static const uint8_t SS_FONT[128][5] = {
    [' ']={0,0,0,0,0},    ['!']={0,0,0x5F,0,0},
    ['0']={0x3E,0x51,0x49,0x45,0x3E},['1']={0,0x42,0x7F,0x40,0},
    ['2']={0x42,0x61,0x51,0x49,0x46},['3']={0x21,0x41,0x45,0x4B,0x31},
    ['4']={0x18,0x14,0x12,0x7F,0x10},['5']={0x27,0x45,0x45,0x45,0x39},
    ['6']={0x3C,0x4A,0x49,0x49,0x30},['7']={0x01,0x71,0x09,0x05,0x03},
    ['8']={0x36,0x49,0x49,0x49,0x36},['9']={0x06,0x49,0x49,0x29,0x1E},
    ['A']={0x7E,0x11,0x11,0x11,0x7E},['B']={0x7F,0x49,0x49,0x49,0x36},
    ['C']={0x3E,0x41,0x41,0x41,0x22},['D']={0x7F,0x41,0x41,0x22,0x1C},
    ['E']={0x7F,0x49,0x49,0x49,0x41},['F']={0x7F,0x09,0x09,0x09,0x01},
    ['G']={0x3E,0x41,0x49,0x49,0x7A},['H']={0x7F,0x08,0x08,0x08,0x7F},
    ['I']={0,0x41,0x7F,0x41,0},    ['J']={0x20,0x40,0x41,0x3F,0x01},
    ['K']={0x7F,0x08,0x14,0x22,0x41},['L']={0x7F,0x40,0x40,0x40,0x40},
    ['M']={0x7F,0x02,0x0C,0x02,0x7F},['N']={0x7F,0x04,0x08,0x10,0x7F},
    ['O']={0x3E,0x41,0x41,0x41,0x3E},['P']={0x7F,0x09,0x09,0x09,0x06},
    ['Q']={0x3E,0x41,0x51,0x21,0x5E},['R']={0x7F,0x09,0x19,0x29,0x46},
    ['S']={0x46,0x49,0x49,0x49,0x31},['T']={0x01,0x01,0x7F,0x01,0x01},
    ['U']={0x3F,0x40,0x40,0x40,0x3F},['V']={0x1F,0x20,0x40,0x20,0x1F},
    ['W']={0x3F,0x40,0x38,0x40,0x3F},['X']={0x63,0x14,0x08,0x14,0x63},
    ['Y']={0x07,0x08,0x70,0x08,0x07},['Z']={0x61,0x51,0x49,0x45,0x43},
    ['a']={0x20,0x54,0x54,0x54,0x78},['b']={0x7F,0x48,0x44,0x44,0x38},
    ['c']={0x38,0x44,0x44,0x44,0x20},['d']={0x38,0x44,0x44,0x48,0x7F},
    ['e']={0x38,0x54,0x54,0x54,0x18},['f']={0x08,0x7E,0x09,0x01,0x02},
    ['g']={0x0C,0x52,0x52,0x52,0x3E},['h']={0x7F,0x08,0x04,0x04,0x78},
    ['i']={0,0x44,0x7D,0x40,0},    ['j']={0x20,0x40,0x44,0x3D,0},
    ['k']={0x7F,0x10,0x28,0x44,0}, ['l']={0,0x41,0x7F,0x40,0},
    ['m']={0x7C,0x04,0x18,0x04,0x78},['n']={0x7C,0x08,0x04,0x04,0x78},
    ['o']={0x38,0x44,0x44,0x44,0x38},['p']={0x7C,0x14,0x14,0x14,0x08},
    ['q']={0x08,0x14,0x14,0x18,0x7C},['r']={0x7C,0x08,0x04,0x04,0x08},
    ['s']={0x48,0x54,0x54,0x54,0x20},['t']={0x04,0x3F,0x44,0x40,0x20},
    ['u']={0x3C,0x40,0x40,0x20,0x7C},['v']={0x1C,0x20,0x40,0x20,0x1C},
    ['w']={0x3C,0x40,0x30,0x40,0x3C},['x']={0x44,0x28,0x10,0x28,0x44},
    ['y']={0x0C,0x50,0x50,0x50,0x3C},['z']={0x44,0x64,0x54,0x4C,0x44},
    ['-']={0x08,0x08,0x08,0x08,0x08},['<']={0x08,0x14,0x22,0x41,0},
    ['>']={0,0x41,0x22,0x14,0x08},  ['.']={0,0x60,0x60,0,0},
    ['(']={0x1C,0x22,0x41,0,0},     [')']={0,0,0x41,0x22,0x1C},
};

// Physical buffer: row 0-799 = landscape X, col 0-479 = landscape Y (high=top)
// Text: row increases per char (left→right), col decreases per font row (top→down)

// Fill a rectangle. memset() for black; 32-bit-at-a-time pixel-pair writes
// for non-zero colors (halves PSRAM transactions vs per-pixel loop).
static inline void ss_rect(uint16_t *p, int r0, int c_top, int rw, int ch, uint16_t col) {
    if (col == 0) {
        for (int r = r0; r < r0 + rw; r++) {
            memset(p + r * 480 + (c_top - ch + 1), 0, ch * 2);
        }
        return;
    }
    uint32_t col32 = ((uint32_t)col << 16) | col;
    int pairs = ch >> 1;
    int rem   = ch & 1;
    for (int r = r0; r < r0 + rw; r++) {
        uint16_t *row = p + r * 480 + (c_top - ch + 1);
        uint32_t *row32 = (uint32_t *)row;          // row start is 4-byte aligned (rows are 480*2 = 960 bytes apart)
        for (int i = 0; i < pairs; i++) row32[i] = col32;
        if (rem) row[ch - 1] = col;
    }
}

// Draw horizontal border line (1 row thick, rw rows wide) at col c
static inline void ss_hline(uint16_t *p, int r0, int c, int rw, uint16_t col) {
    for (int r = r0; r < r0 + rw; r++) p[r * 480 + c] = col;
}

// Draw vertical border (ch cols tall) at row r
static inline void ss_vline(uint16_t *p, int r, int c_top, int ch, uint16_t col) {
    for (int c = c_top - ch + 1; c <= c_top; c++) p[r * 480 + c] = col;
}

static void ss_text(uint16_t *p, const char *s, int row, int col, int sc, uint16_t color) {
    for (int ci = 0; s[ci]; ci++) {
        unsigned char ch = (unsigned char)s[ci];
        if (ch >= 128) { row += 6*sc; continue; }
        for (int fx = 0; fx < 5; fx++) {
            uint8_t bits = SS_FONT[ch][fx];
            for (int fy = 0; fy < 7; fy++) {
                if (bits & (1 << fy)) {
                    for (int sy = 0; sy < sc; sy++)
                    for (int sx = 0; sx < sc; sx++) {
                        int pr = row + fx*sc + sy;
                        int pc = col - fy*sc - sx;
                        if (pr >= 0 && pr < 800 && pc >= 0 && pc < 480)
                            p[pr * 480 + pc] = color;
                    }
                }
            }
        }
        row += 6*sc;
    }
}

static void draw_scale_menu(uint8_t *buf) {
    uint16_t *p = (uint16_t *)buf;
    const int SC = 2, CW = 6*SC, CH = 8*SC;
    const int R0=SM_R0, RW=SM_RW, C0=SM_C0, BH=SM_BH;

    ss_rect(p, R0, C0, RW, BH, 0x1083);
    ss_hline(p, R0,      C0,      RW, 0xF800);
    ss_hline(p, R0,      C0-BH+1, RW, 0xF800);
    ss_vline(p, R0,      C0,      BH, 0xF800);
    ss_vline(p, R0+RW-1, C0,      BH, 0xF800);
    ss_text(p, "SCALE", R0+8, C0-8, SC, 0xFFFF);

    const char *slabels[SCALE_COUNT] = {"STRETCH", "FIT", "3X"};
    for (int i = 0; i < SCALE_COUNT; i++) {
        int ic = C0 - 8 - (CH+8)*(i+1);
        if (scale_cursor == i) ss_text(p, ">",           R0+8,      ic, SC, 0xF800);
        uint16_t tcol = (scale_mode == i) ? 0xF800 : 0xFFFF;
        ss_text(p, slabels[i], R0+8+CW*2, ic, SC, tcol);
    }
}

static void draw_layout_menu(uint8_t *buf) {
    uint16_t *p = (uint16_t *)buf;
    const int SC = 2, CW = 6*SC, CH = 8*SC;
    const int R0=LM_R0, RW=LM_RW, C0=LM_C0, BH=LM_BH;

    ss_rect(p, R0, C0, RW, BH, 0x1083);
    ss_hline(p, R0,       C0,       RW, 0xF800);
    ss_hline(p, R0,       C0-BH+1,  RW, 0xF800);
    ss_vline(p, R0,       C0,       BH, 0xF800);
    ss_vline(p, R0+RW-1,  C0,       BH, 0xF800);
    ss_text(p, "LAYOUT", R0+8, C0-8, SC, 0xFFFF);

    const char *labels[] = {"Default", "WASD"};
    for (int i = 0; i < 2; i++) {
        int ic = C0 - 8 - (CH+8)*(i+1);
        if (lm_cursor == i) ss_text(p, ">", R0+8, ic, SC, 0xF800);
        uint16_t tcol = (key_layout == i) ? 0xF800 : 0xFFFF;
        ss_text(p, labels[i], R0+8+CW*2, ic, SC, tcol);
    }
}


// Pre-rendered menu background cache
static uint8_t *ss_bg_cache = NULL;
static int      ss_bg_valid = 0;

static void draw_ss_menu(uint8_t *buf) {
    uint16_t *p = (uint16_t *)buf;
    const int SC = 2, CW = 6*SC, CH = 8*SC;
    const int R0=SS_MENU_R0, C0=SS_MENU_C0, RW=SS_MENU_RW, BH=SS_MENU_BH;

    // Dark background panel + green border for visibility
    ss_rect(p, R0, C0, RW, BH, 0x1083);
    ss_hline(p, R0,       C0,       RW, 0xF800);
    ss_hline(p, R0,       C0-BH+1,  RW, 0xF800);
    ss_vline(p, R0,       C0,       BH, 0xF800);
    ss_vline(p, R0+RW-1,  C0,       BH, 0xF800);
    ss_text(p, "SAVE STATE", R0+8, C0-8, SC, 0xFFFF);

    char slot_str[24];
    snprintf(slot_str, sizeof(slot_str), "< Slot %d >", (int)ss_slot);
    ss_text(p, slot_str, R0+8, C0-8-CH-8, SC, 0xF800);
    ss_text(p, ss_exists[ss_slot] ? "SAVED" : "EMPTY",
            R0+8+11*CW, C0-8-CH-8, SC, ss_exists[ss_slot] ? 0xF800 : 0x8410);

    const char *items[4] = {"SAVE", "LOAD", "DELETE", "CANCEL"};
    for (int i = 0; i < 4; i++) {
        int ic = C0 - 8 - (CH+8)*(i+2) - 8;
        uint16_t icol;
        if ((i == SS_LOAD || i == SS_DELETE) && !ss_exists[ss_slot])
            icol = 0x8410; // dimmed — slot empty
        else if (i == SS_DELETE && ss_exists[ss_slot])
            icol = 0xF800; // red — destructive action
        else
            icol = 0xFFFF;
        if (ss_cursor == i) ss_text(p, ">", R0+8, ic, SC, 0xF800);
        ss_text(p, items[i], R0+8+CW*2, ic, SC, icol);
    }

    if (ss_toast_f > 0) {
        ss_toast_f--;
        // One slot below CANCEL so it never overlaps menu items
        int toast_col = C0 - 8 - (CH+8)*6 - 8;
        ss_text(p, ss_toast, R0+6, toast_col, SC, 0xFFFF);
    }
}
static void ss_io_task(void *arg) {
    for (;;) {
        xSemaphoreTake(sem_ss, portMAX_DELAY);
        int op = ss_io_op, slot = ss_slot;
        char path[340];
        snprintf(path, sizeof(path), "%s.ss%d", state_save_dir, slot);
        if (op == 1) {
            FILE *f = fopen(path, "wb");
            if (f) { savestate(f); fclose(f); ss_exists[slot] = true;
                snprintf(ss_toast, sizeof(ss_toast), "Slot %d saved!", slot);
                ESP_LOGI("howboymatsu", "State saved: %s", path);
            } else { snprintf(ss_toast, sizeof(ss_toast), "Save failed!"); }
        } else if (op == 2) {
            FILE *f = fopen(path, "rb");
            if (f) { loadstate(f); fclose(f);
                vram_dirty(); pal_dirty(); sound_dirty(); mem_updatemap();
                memset(pcm.buf, 0, pcm.len * sizeof(int16_t)); pcm.pos = 0;
                snprintf(ss_toast, sizeof(ss_toast), "Slot %d loaded!", slot);
                ESP_LOGI("howboymatsu", "State loaded: %s", path);
            } else { snprintf(ss_toast, sizeof(ss_toast), "Slot %d empty!", slot); }
        } else if (op == 3) {
            if (remove(path) == 0) {
                ss_exists[slot] = false;
                snprintf(ss_toast, sizeof(ss_toast), "Slot %d deleted!", slot);
                ESP_LOGI("howboymatsu", "State deleted: %s", path);
            } else { snprintf(ss_toast, sizeof(ss_toast), "Delete failed!"); }
        }
        ss_toast_f = 120;
        ss_io_op = 0;
        ss_drawn_static = 0;
        ss_clear_region  = 3;
        ss_state = SS_MENU_CLOSED;
        ss_menu_invalidate();
    }
}

// --- Main emulator task ---
void blit_task(void *arg) {
    const int PHYS_W = 480;
    const int PHYS_H = 800;
    for (;;) {
        xSemaphoreTake(sem_frame_ready, portMAX_DELAY);
        uint8_t *buf = (active_render_buf == 0) ? render_buf_a : render_buf_b;
        // Draw FPS overlay directly into render buffer
        if (show_fps && current_fps > 1.0f) {
            static const uint8_t font5x7[][5] = {
                {0x1F,0x11,0x11,0x11,0x1F},{0x00,0x12,0x1F,0x10,0x00},
                {0x1D,0x15,0x15,0x15,0x17},{0x11,0x15,0x15,0x15,0x1F},
                {0x07,0x04,0x04,0x04,0x1F},{0x17,0x15,0x15,0x15,0x1D},
                {0x1F,0x15,0x15,0x15,0x1D},{0x01,0x01,0x01,0x01,0x1F},
                {0x1F,0x15,0x15,0x15,0x1F},{0x17,0x15,0x15,0x15,0x1F},
                {0x00,0x18,0x18,0x00,0x00},{0x00,0x00,0x00,0x00,0x00},
                {0x1F,0x05,0x05,0x05,0x01},{0x1F,0x15,0x15,0x09,0x00},
                {0x11,0x15,0x15,0x15,0x1B}
            };
            char fps_str[16];
            snprintf(fps_str, sizeof(fps_str), "%.1f", current_fps);
            uint16_t *phys = (uint16_t *)buf;
            uint16_t red = 0xF800;
            // Scale factor for text size
            const int SC = 3;
            // Text origin in physical buffer: top-left corner of game screen
            // Game screen is at col X_OFF=24, rows 0-800
            // We want text at top-left of the game area
            // In rotated buffer: game cols map to physical cols 24..455
            // Text at physical row=4, col=28 (just inside game area)
            const int TY = 4;   // physical row (0-799)
            const int TX = 28;  // physical col (0-479)
            int text_w = 8 * SC * 6; // 8 chars * SC * 6px wide
            int text_h = 7 * SC + 4;
            // Top-right in landscape = high phys row, low phys col
            // Text goes right = phys row increases per char
            // Font col = row advance, font row = col advance (downward = col increases)
            // Render FPS text: top-right, horizontal, correct orientation
            // Physical buffer: row=0 is landscape-left, row=799 is landscape-right
            // Physical buffer: col=0 is landscape-bottom, col=479 is landscape-top
            // Top-right = high row, low col. Text left-to-right = row decreases per char.
            // Font: col index = along text direction (row axis), row index = vertical (col axis)
            int fps_len = 0;
            while (fps_str[fps_len]) fps_len++;
            int start_row = 790;  // rightmost char starts here
            for (int ci = 0; ci < fps_len; ci++) {
                char ch = fps_str[ci];
                int idx = -1;
                if (ch>='0'&&ch<='9') idx=ch-'0';
                else if (ch=='.') idx=10;
                else if (ch==' ') idx=11;
                else if (ch=='F') idx=12;
                else if (ch=='P') idx=13;
                else if (ch=='S') idx=14;
                // char position: rightmost char = start_row, leftmost = start_row - (fps_len-1)*6*SC
                int char_row = start_row - (fps_len - 1 - ci) * 6 * SC;
                if (idx >= 0) {
                    for (int col=0; col<5; col++) {
                        uint8_t bits = font5x7[idx][col];
                        for (int row=0; row<7; row++) {
                            if (bits & (1<<row)) {
                                for (int sy2=0; sy2<SC; sy2++)
                                for (int sx2=0; sx2<SC; sx2++) {
                                    int px = char_row - (4-col)*SC - sy2;
                                    int py = 4 + (6-row)*SC + sx2;
                                    if (px>=0 && px<800 && py>=0 && py<480)
                                        phys[px * PHYS_W + py] = red;
                                }
                            }
                        }
                    }
                }
            }
        }
        // Save state menu overlay. Only redraw when the current buffer doesn't
        // already have the menu (just opened, state changed) or while a toast
        // is animating. vid_end skips the menu rect when the menu is open, so
        // the menu pixels persist across frames without being overwritten.
        if (ss_state == SS_MENU_OPEN || ss_state == SS_MENU_SAVING || ss_state == SS_MENU_LOADING) {
            int dirty = (active_render_buf == 0) ? !ss_menu_drawn_a : !ss_menu_drawn_b;
            if (dirty || ss_toast_f > 0) {
                int64_t t0 = esp_timer_get_time();
                draw_ss_menu(buf);
                int64_t t1 = esp_timer_get_time();
                ESP_LOGI("howboymatsu", "menu draw us: %lld (buf=%c)", (long long)(t1-t0),
                         active_render_buf == 0 ? 'A' : 'B');
                if (active_render_buf == 0) ss_menu_drawn_a = 1;
                else                        ss_menu_drawn_b = 1;
            }
        } else {
            ss_menu_drawn_a = 0;
            ss_menu_drawn_b = 0;
        }
        // Layout menu overlay (same persistent-render pattern as save state menu)
        if (layout_menu_open) {
            int lm_dirty = (active_render_buf == 0) ? !lm_drawn_a : !lm_drawn_b;
            if (lm_dirty) {
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
            int sm_dirty = (active_render_buf == 0) ? !scale_drawn_a : !scale_drawn_b;
            if (sm_dirty) {
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
void emulator_task(void *arg) {
    // Skip SD mount if already mounted (return_to_selector loop)
    static int sd_mounted = 0;
    if (sd_mounted) goto sd_already_mounted;
    ESP_LOGI(TAG, "Mounting SD card...");

sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = BSP_SDCARD_CLK;
    slot_config.cmd = BSP_SDCARD_CMD;
    slot_config.d0  = BSP_SDCARD_D0;
    slot_config.d1  = BSP_SDCARD_D1;
    slot_config.d2  = BSP_SDCARD_D2;
    slot_config.d3  = BSP_SDCARD_D3;
    slot_config.width = 4;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t *card;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        restart_to_launcher();
    }
    ESP_LOGI(TAG, "SD card mounted at /sdcard");
    sd_mounted = 1;
    sd_already_mounted:;

    // Diagnostic - list what's on the SD card
  DIR *romsdir = opendir("/sdcard/roms");
    if (romsdir) {
        ESP_LOGI(TAG, "Contents of /sdcard/roms:");
        struct dirent *entry;
        while ((entry = readdir(romsdir)) != NULL) {
            ESP_LOGI(TAG, "  %s", entry->d_name);
        }
        closedir(romsdir);
    } else {
        ESP_LOGE(TAG, "Cannot open /sdcard/roms directory!");
    }
    ESP_LOGI(TAG, "Initializing gnuboy...");

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
        while (1) {
            bsp_input_event_t ev2;
            if (xQueueReceive(input_event_queue, &ev2, portMAX_DELAY) == pdTRUE)
                if (ev2.type == INPUT_EVENT_TYPE_NAVIGATION && ev2.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_ESC && ev2.args_navigation.state == 1)
                    restart_to_launcher();
        }
    }
    ESP_LOGI(TAG, "Loading ROM from %s", rom_path);

    // Read ROM file into memory
    FILE *rom_fd = fopen(rom_path, "rb");
    if (rom_fd == NULL) {
        ESP_LOGE(TAG, "Failed to open ROM file: %s", rom_path);
        pax_background(&fb_pax, 0xFF000000);
        pax_draw_text(&fb_pax, 0xFFFF0000, pax_font_sky_mono, 16, 10, 10, "ROM file not found!");
        pax_draw_text(&fb_pax, 0xFFFFFF00, pax_font_sky_mono, 12, 10, 40, rom_path);
        pax_draw_text(&fb_pax, 0xFFAAAAAA, pax_font_sky_mono, 12, 10, 70, "Press ESC to return");
        blit();
        bsp_input_event_t ev;
        while (1) {
            if (xQueueReceive(input_event_queue, &ev, portMAX_DELAY) == pdTRUE) {
                if (ev.type == INPUT_EVENT_TYPE_NAVIGATION &&
                    ev.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_ESC) {
                    restart_to_launcher();
                }
            }
        }
    }

    // Get file size
// Count file size by reading to end
    size_t rom_length = 0;
    char count_buf[512];
    while (fread(count_buf, 1, sizeof(count_buf), rom_fd) > 0) {
        rom_length += sizeof(count_buf);
    }
    // Correct for possible short last read
    fseek(rom_fd, 0, SEEK_END);
    rom_length = (size_t)ftell(rom_fd);
    // If ftell still fails, use manual count
    if (rom_length == 0) {
        rewind(rom_fd);
        size_t n;
        rom_length = 0;
        while ((n = fread(count_buf, 1, sizeof(count_buf), rom_fd)) > 0)
            rom_length += n;
    }
    fseek(rom_fd, 0, SEEK_SET);
    ESP_LOGI(TAG, "ROM size: %d bytes", rom_length);

    // Allocate memory in PSRAM for the ROM
    uint8_t *rom_data = (uint8_t *)heap_caps_malloc(rom_length, MALLOC_CAP_SPIRAM);
    if (rom_data == NULL) {
        // Fall back to regular RAM if PSRAM fails
        rom_data = (uint8_t *)malloc(rom_length);
    }
    if (rom_data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes for ROM", rom_length);
        fclose(rom_fd);
        restart_to_launcher();
    }

    // Read ROM into memory
    fread(rom_data, 1, rom_length, rom_fd);
    fclose(rom_fd);
    ESP_LOGI(TAG, "ROM loaded into RAM at %p", rom_data);

    // Pass ROM data to gnuboy
    loader_init(rom_data);

    // Derive SRAM save path from ROM path
    mkdir("/sdcard/saves", 0777);
    const char *rom_base = strrchr(rom_path, '/');
    rom_base = rom_base ? rom_base + 1 : rom_path;
    snprintf(sram_path_global, sizeof(sram_path_global), "/sdcard/saves/%s", rom_base);
    char *dot = strrchr(sram_path_global, '.');
    if (dot) strcpy(dot, ".sav");

    // Derive state save base path
    snprintf(state_save_dir, sizeof(state_save_dir), "/sdcard/saves/%s", rom_base);
    char *sdot = strrchr(state_save_dir, '.');
    if (sdot) *sdot = '\0';
    ESP_LOGI(TAG, "State save dir: %s", state_save_dir);

    // Load SRAM on startup
    ESP_LOGI(TAG, "SRAM path: %s", sram_path_global);
    FILE *sram_f = fopen(sram_path_global, "rb");
    if (sram_f) {
        int r = sram_load(sram_f);
        fclose(sram_f);
        // Load RTC data
        char rtc_path[320];
        strncpy(rtc_path, sram_path_global, sizeof(rtc_path)-1);
        char *rdot = strrchr(rtc_path, '.');
        if (rdot) strcpy(rdot, ".rtc");
        FILE *rtc_f = fopen(rtc_path, "rb");
        if (rtc_f) { rtc_load(rtc_f); }  // rtc_load closes the file internally
        else rtc_load(NULL);
        ESP_LOGI(TAG, "SRAM loaded from %s (ret=%d)", sram_path_global, r);
    } else {
        int r = sram_load(NULL);
        ESP_LOGI(TAG, "No SRAM save found, starting fresh (ret=%d)", r);
    }

    sram_load_done = true;
    sram_load_done = true;
    ESP_LOGI(TAG, "ROM loaded! Starting emulator...");
    pax_background(&fb_pax, 0xFF000000);
    blit();

    emu_reset();
    // Refresh state after SRAM load
    if (sram_path_global[0]) {
        vram_dirty();
        pal_dirty();
        sound_dirty();
        mem_updatemap();
    }
    // Unmute audio now that ROM is loaded and ready to run
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

    // Allocate rewind buffers in PSRAM (one-time; persists across ROM reloads)
    if (!rewind_state_buf) {
        rewind_state_buf = heap_caps_malloc((size_t)REWIND_SLOTS * REWIND_STATE_SZ, MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "Rewind state buf: %s (%u KB)",
                 rewind_state_buf ? "OK" : "FAIL",
                 (unsigned)(REWIND_SLOTS * REWIND_STATE_SZ / 1024));
    }
    if (!rewind_pix_buf) {
        rewind_pix_buf = heap_caps_malloc(
            (size_t)REWIND_SLOTS * GBC_WIDTH * GBC_HEIGHT * sizeof(uint16_t),
            MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "Rewind pix buf:   %s (%u KB)",
                 rewind_pix_buf ? "OK" : "FAIL",
                 (unsigned)(REWIND_SLOTS * GBC_WIDTH * GBC_HEIGHT * 2 / 1024));
    }
    if (!rewind_sram_backup) {
        // Max GBC SRAM = 16 banks × 8192 bytes = 128 KB
        rewind_sram_backup = heap_caps_malloc(16 * 8192, MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "Rewind SRAM backup: %s (128 KB)", rewind_sram_backup ? "OK" : "FAIL");
    }
    // Reset rewind state for new ROM session
    rw_head = 0; rw_count = 0; rw_pos = 0; rw_frame_ctr = 0; rewind_active = 0; audio_mute = 0;
    memset(rw_sizes, 0, sizeof(rw_sizes));

    emu_run();

    vTaskDelete(NULL);
}

// --- App entry point ---
void audio_task(void *arg) {
    i2s_chan_handle_t i2s = NULL;
    bsp_audio_get_i2s_handle(&i2s);
    static int16_t silence[4096] = {0};
    size_t written = 0;
    while (1) {
        xSemaphoreTake(sem_audio_ready, portMAX_DELAY);
        if (audio_mute) {
            if (i2s && i2s_enabled)
                i2s_channel_write(i2s, silence, pcm.len * sizeof(int16_t), &written, pdMS_TO_TICKS(100));
            if (sem_audio_shutdown) xSemaphoreGive(sem_audio_shutdown);
            xSemaphoreGive(sem_audio_done);
            continue;
        }
        int16_t *buf = (audio_buf_ready == 0) ? audio_buf_b : audio_buf_a;
        if (i2s && i2s_enabled) i2s_channel_write(i2s, buf, audio_buf_len * sizeof(int16_t), &written, pdMS_TO_TICKS(100));
        xSemaphoreGive(sem_audio_done);
    }
}
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
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "BSP init failed: %d", res);
        return;
    }

    res = bsp_display_get_parameters(&display_h_res, &display_v_res,
                                      &display_color_format, &display_data_endian);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Display parameters failed: %d", res);
        return;
    }

    pax_buf_type_t format = PAX_BUF_24_888RGB;
    if (display_color_format == LCD_COLOR_PIXEL_FORMAT_RGB565) {
        format = PAX_BUF_16_565RGB;
    }
    bsp_display_rotation_t display_rotation = BSP_DISPLAY_ROTATION_90;
    pax_orientation_t orientation = PAX_O_UPRIGHT;
    switch (display_rotation) {
    case BSP_DISPLAY_ROTATION_90: orientation = PAX_O_ROT_CW; break;
    case BSP_DISPLAY_ROTATION_180: orientation = PAX_O_ROT_HALF; break;
    case BSP_DISPLAY_ROTATION_270: orientation = PAX_O_ROT_CCW; break;
    default: orientation = PAX_O_UPRIGHT; break;
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

// Allocate display buffer in PSRAM
    display_buf = (uint8_t *)heap_caps_malloc(display_h_res * display_v_res * 3, MALLOC_CAP_SPIRAM);
    render_buf_a = (uint8_t *)heap_caps_malloc(480 * 800 * 2, MALLOC_CAP_SPIRAM);
    render_buf_b = (uint8_t *)heap_caps_malloc(480 * 800 * 2, MALLOC_CAP_SPIRAM);
    if (render_buf_a) memset(render_buf_a, 0, 480 * 800 * 2);
    if (render_buf_b) memset(render_buf_b, 0, 480 * 800 * 2);
    ESP_LOGI(TAG, "display_buf=%p render_a=%p render_b=%p", display_buf, render_buf_a, render_buf_b);
    if (!render_buf_a || !render_buf_b) { ESP_LOGE(TAG, "Failed to allocate render buffers!"); return; }
    sem_frame_ready = xSemaphoreCreateBinary();
    sem_frame_done  = xSemaphoreCreateBinary();
    xSemaphoreGive(sem_frame_done);
    if (!display_buf) {
        ESP_LOGE(TAG, "Failed to allocate display buffer!");
        return;
    }
    memset(display_buf, 0, display_h_res * display_v_res * 3);
    ss_bg_cache = heap_caps_malloc(220 * 210 * 2, MALLOC_CAP_SPIRAM);
    sem_ss = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(ss_io_task, "ss_io", 8192, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(blit_task, "blit", 8192, NULL, 5, &blit_task_handle, 0);
    // Main loop: supports returning to ROM selector without hardware restart
    while (1) {
        sem_emulator_done = xSemaphoreCreateBinary();
        return_to_selector = 0;
        ff_speed = 0;
        if (blit_task_handle) vTaskResume(blit_task_handle);
        xTaskCreatePinnedToCore(emulator_task, "emulator", 32768, NULL, 5, &emulator_task_handle, 1);
        xSemaphoreTake(sem_emulator_done, portMAX_DELAY);
        vSemaphoreDelete(sem_emulator_done);
        sem_emulator_done = NULL;
        emulator_task_handle = NULL;
        if (!return_to_selector) break;
        // Audio is already muted via codec volume=0 and amplifier=false
        // Just reset pcm state for clean start
        if (pcm.buf) { memset(pcm.buf, 0, pcm.len * sizeof(int16_t)); pcm.pos = 0; }
        sound_dirty();
    }
}
