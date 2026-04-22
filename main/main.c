#include <stdio.h>
#include <string.h>
#include "bsp/device.h"
#include "bsp/display.h"
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

// gnuboy headers
#include "gnuboy.h"
#include "loader.h"
#include "fb.h"
#include "rc.h"
#include "input.h"
#include "pcm.h"
#include "lcd.h"

// gnuboy global variables required by the emulator core
struct fb fb;
struct pcm pcm;

// Display buffers gnuboy uses internally
uint16_t* displayBuffer[2];
uint8_t currentBuffer = 0;

// Frame buffer that gnuboy's lcd.c writes scan lines into
static uint16_t frame_buf[160 * 144];
uint16_t* frame = frame_buf;

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
#define ROM_PATH "/sdcard/roms/game.gb"

// GBC screen size
#define GBC_WIDTH  160
#define GBC_HEIGHT 144

// Display scale factor (how many Tanmatsu pixels per GBC pixel)
#define SCALE 3

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
void vid_end(void);
void vid_setpal(int i, int r, int g, int b);
void pcm_init(void);
int  pcm_submit(void);
void sys_sleep(int us);
void doevents(void);

// Blit PAX framebuffer to display
void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb_pax));
}

// Draw the GBC screen scaled up onto the Tanmatsu display
void draw_gbc_screen(void) {
    // Center the scaled GBC image on the 800x480 display
    int offset_x = (display_h_res - GBC_WIDTH  * SCALE) / 2;
    int offset_y = (display_v_res - GBC_HEIGHT * SCALE) / 2;

    for (int y = 0; y < GBC_HEIGHT; y++) {
        for (int x = 0; x < GBC_WIDTH; x++) {
            uint16_t pixel = gbc_pixels[y * GBC_WIDTH + x];
            // Convert RGB565 to ARGB for PAX
            uint8_t r = ((pixel >> 11) & 0x1F) << 3;
            uint8_t g = ((pixel >> 5)  & 0x3F) << 2;
            uint8_t b = ( pixel        & 0x1F) << 3;
            pax_col_t col = 0xFF000000 | (r << 16) | (g << 8) | b;
            // Draw scaled pixel as a filled rectangle
            pax_simple_rect(&fb_pax, col,
                offset_x + x * SCALE,
                offset_y + y * SCALE,
                SCALE, SCALE);
        }
    }
    blit();
}

// --- gnuboy platform callbacks ---

// Called by gnuboy to set up video
void vid_preinit(void) {}

void vid_init(void) {
    // Point gnuboy's framebuffer at our pixel buffer
    fb.ptr     = (uint8_t *)gbc_pixels;
    fb.w       = GBC_WIDTH;
    fb.h       = GBC_HEIGHT;
    fb.pelsize = 2;          // 2 bytes per pixel = RGB565
    fb.pitch   = GBC_WIDTH * 2;
    fb.indexed = 0;
    fb.enabled = 1;
    fb.dirty   = 0;
    // Set RGB565 bit layout
    fb.cc[0].r = 11; fb.cc[0].l = 0; // Red:   bits 15-11
    fb.cc[1].r = 5;  fb.cc[1].l = 0; // Green: bits 10-5
    fb.cc[2].r = 0;  fb.cc[2].l = 0; // Blue:  bits 4-0
    fb.cc[3].r = 0;  fb.cc[3].l = 0; // Alpha: unused
    ESP_LOGI(TAG, "gnuboy video initialized");
}

void vid_close(void) {}

void vid_begin(void) {
    fb.dirty = 0;
}

void vid_end(void) {
    // A frame is ready — draw it to the Tanmatsu screen
    if (fb.dirty) {
        draw_gbc_screen();
    }
}

void vid_setpal(int i, int r, int g, int b) {
    (void)i; (void)r; (void)g; (void)b;
}

void vid_settitle(char *title) {
    ESP_LOGI(TAG, "Game title: %s", title);
}

// --- gnuboy audio callbacks (stub for now) ---
void pcm_init(void)    {}
int  pcm_submit(void)  { return 0; }
void pcm_close(void)   {}

// --- gnuboy system callbacks ---
void sys_sleep(int us) {
    vTaskDelay(pdMS_TO_TICKS(us / 1000));
}

void *sys_timer(void) { return NULL; }
int   sys_elapsed(void *ptr) { (void)ptr; return 0; }

void sys_checkdir(char *path, int wr) { (void)path; (void)wr; }
void sys_sanitize(char *s)            { (void)s; }
void sys_initpath(char *exe)          { (void)exe; }

// --- gnuboy input ---
// Map Tanmatsu keys to GBC buttons
// GBC buttons: up=0 down=1 left=2 right=3 a=4 b=5 start=6 select=7
static int gbc_keys[8] = {0};

void doevents(void) {
    bsp_input_event_t event;
    // Drain all pending input events
    while (xQueueReceive(input_event_queue, &event, 0) == pdTRUE) {
        int pressed = 0;
        int key = -1;

        if (event.type == INPUT_EVENT_TYPE_NAVIGATION) {
            pressed = event.args_navigation.state;
            switch (event.args_navigation.key) {
                case BSP_INPUT_NAVIGATION_KEY_UP:    key = 0; break; // Up
                case BSP_INPUT_NAVIGATION_KEY_DOWN:  key = 1; break; // Down
                case BSP_INPUT_NAVIGATION_KEY_LEFT:  key = 2; break; // Left
                case BSP_INPUT_NAVIGATION_KEY_RIGHT: key = 3; break; // Right
                case BSP_INPUT_NAVIGATION_KEY_F1:    // F1 = return to launcher
                    if (pressed) bsp_device_restart_to_launcher();
                    break;
                default: break;
            }
        } else if (event.type == INPUT_EVENT_TYPE_KEYBOARD) {
            pressed = 1; // key down
            switch (event.args_keyboard.ascii) {
                case 'x': case 'X': key = 4; break; // A button
                case 'z': case 'Z': key = 5; break; // B button
                case '\n':          key = 6; break; // Start (Enter)
                case ' ':           key = 7; break; // Select (Space)
                default: break;
            }
        }

        if (key >= 0) {
            gbc_keys[key] = pressed;
            // Tell gnuboy about the key state
            // rc_dokey maps key codes to GBC buttons
            rc_dokey(key, pressed);
        }
    }
}

void ev_poll(void) {
    doevents();
}

// gnuboy die() handler
void die(char *fmt, ...) {
    va_list ap;
    char buf[256];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "%s", buf);
    abort();
}

// --- Main emulator task ---
void emulator_task(void *arg) {
    ESP_LOGI(TAG, "Initializing gnuboy...");

    // Initialize gnuboy exports/variables
    init_exports();

    // Set up default key bindings
    rc_command("bind up +up");
    rc_command("bind down +down");
    rc_command("bind left +left");
    rc_command("bind right +right");
    rc_command("bind a +a");
    rc_command("bind b +b");
    rc_command("bind start +start");
    rc_command("bind select +select");

    // Initialize video and audio
    vid_init();
    pcm_init();

    // Load the ROM
    ESP_LOGI(TAG, "Loading ROM from %s", ROM_PATH);
    loader_init((uint8_t *)ROM_PATH);

    int result = rom_load();
    if (result != 0) {
        ESP_LOGE(TAG, "Failed to load ROM! Make sure %s exists on your SD card.", ROM_PATH);
        // Show error on screen
        pax_background(&fb_pax, 0xFF000000);
        pax_draw_text(&fb_pax, 0xFFFFFFFF, pax_font_sky_mono, 16, 10, 10,  "HowBoyMatsu");
        pax_draw_text(&fb_pax, 0xFFFF0000, pax_font_sky_mono, 14, 10, 40,  "ROM not found!");
        pax_draw_text(&fb_pax, 0xFFFFFFFF, pax_font_sky_mono, 12, 10, 70,  "Put a .gb or .gbc file at:");
        pax_draw_text(&fb_pax, 0xFFFFFF00, pax_font_sky_mono, 12, 10, 90,  ROM_PATH);
        pax_draw_text(&fb_pax, 0xFFAAAAAA, pax_font_sky_mono, 12, 10, 130, "Press F1 to return to launcher");
        blit();
        // Wait for F1 to go back to launcher
        bsp_input_event_t event;
        while (1) {
            if (xQueueReceive(input_event_queue, &event, portMAX_DELAY) == pdTRUE) {
                if (event.type == INPUT_EVENT_TYPE_NAVIGATION &&
                    event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_F1) {
                    bsp_device_restart_to_launcher();
                }
            }
        }
    }

    ESP_LOGI(TAG, "ROM loaded! Starting emulator...");

    // Black screen while starting
    pax_background(&fb_pax, 0xFF000000);
    blit();

    // Reset and run the emulator
    emu_reset();
    emu_run(); // This loop runs forever

    vTaskDelete(NULL);
}

// --- App entry point ---
void app_main(void) {
    // Start GPIO interrupt service
    gpio_install_isr_service(0);

    // Initialize NVS
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Initialize BSP
    const bsp_configuration_t bsp_configuration = {
        .display = {
            .requested_color_format = LCD_COLOR_PIXEL_FORMAT_RGB888,
            .num_fbs                = 1,
        },
    };
    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "BSP init failed: %d", res);
        return;
    }

    // Get display parameters
    res = bsp_display_get_parameters(&display_h_res, &display_v_res,
                                      &display_color_format, &display_data_endian);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Display parameters failed: %d", res);
        return;
    }

    // Set up PAX graphics buffer
    pax_buf_type_t format = PAX_BUF_24_888RGB;
    if (display_color_format == LCD_COLOR_PIXEL_FORMAT_RGB565) {
        format = PAX_BUF_16_565RGB;
    }
    bsp_display_rotation_t display_rotation = bsp_display_get_default_rotation();
    pax_orientation_t orientation = PAX_O_UPRIGHT;
    switch (display_rotation) {
        case BSP_DISPLAY_ROTATION_90:  orientation = PAX_O_ROT_CCW;  break;
        case BSP_DISPLAY_ROTATION_180: orientation = PAX_O_ROT_HALF; break;
        case BSP_DISPLAY_ROTATION_270: orientation = PAX_O_ROT_CW;   break;
        default:                       orientation = PAX_O_UPRIGHT;  break;
    }
    pax_buf_init(&fb_pax, NULL, display_h_res, display_v_res, format);
    pax_buf_reversed(&fb_pax, display_data_endian == LCD_RGB_DATA_ENDIAN_BIG);
    pax_buf_set_orientation(&fb_pax, orientation);

    // Get input queue
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    // Show splash screen
    pax_background(&fb_pax, 0xFF000000);
    pax_draw_text(&fb_pax, 0xFF00FF00, pax_font_sky_mono, 24, 10, 10,  "HowBoyMatsu");
    pax_draw_text(&fb_pax, 0xFFFFFFFF, pax_font_sky_mono, 14, 10, 50,  "Game Boy Color Emulator");
    pax_draw_text(&fb_pax, 0xFFAAAAAA, pax_font_sky_mono, 12, 10, 80,  "for Tanmatsu");
    pax_draw_text(&fb_pax, 0xFFFFFF00, pax_font_sky_mono, 12, 10, 120, "Loading ROM...");
    blit();

    vTaskDelay(pdMS_TO_TICKS(500));

    // Launch emulator in its own task with plenty of stack space
    xTaskCreatePinnedToCore(
        emulator_task,
        "emulator",
        32768,   // 32KB stack
        NULL,
        5,       // Priority
        NULL,
        1        // Core 1
    );
}
