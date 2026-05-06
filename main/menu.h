#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "config.h"

// ── Save state menu state (defined in menu.c) ────────────────────────────────
extern volatile int      ss_state;
extern volatile int      ss_slot;
extern volatile int      ss_cursor;
extern bool              ss_exists[10];
extern char              ss_toast[32];
extern int               ss_toast_f;
extern volatile int      ss_io_op;
extern SemaphoreHandle_t sem_ss;
extern volatile int      ss_menu_drawn_a;
extern volatile int      ss_menu_drawn_b;
extern volatile int      ss_clear_region;

// ── Scale menu state (defined in menu.c) ─────────────────────────────────────
extern volatile int  scale_mode;
extern volatile int  scale_menu_open;
extern volatile int  scale_cursor;
extern volatile int  scale_drawn_a;
extern volatile int  scale_drawn_b;
extern volatile int  scale_border_dirty;

// ── Layout menu state (defined in menu.c) ────────────────────────────────────
extern volatile int  key_layout;
extern volatile int  layout_menu_open;
extern volatile int  lm_cursor;
extern volatile int  lm_drawn_a;
extern volatile int  lm_drawn_b;

// ── Invalidation helpers ──────────────────────────────────────────────────────
void ss_menu_invalidate(void);
void scale_invalidate(void);
void lm_invalidate(void);

// ── Draw functions ────────────────────────────────────────────────────────────
void draw_ss_menu(uint8_t *buf);
void draw_layout_menu(uint8_t *buf);
void draw_scale_menu(uint8_t *buf);

// ── Background IO task ────────────────────────────────────────────────────────
void ss_io_task(void *arg);
