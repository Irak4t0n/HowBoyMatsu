#pragma once

// GBC screen dimensions
#define GBC_WIDTH  160
#define GBC_HEIGHT 144

// Physical display buffer dimensions (portrait layout, landscape display)
#define PHYS_W 480
#define PHYS_H 800

// Save state menu state machine values
#define SS_MENU_CLOSED  0
#define SS_MENU_OPEN    1
#define SS_MENU_SAVING  2
#define SS_MENU_LOADING 3

// Save state cursor op codes
#define SS_SAVE   0
#define SS_LOAD   1
#define SS_DELETE 2
#define SS_CANCEL 3

// Save state menu rect (physical coords: row=landscape X, col=landscape Y, high=top)
#define SS_MENU_R0      560
#define SS_MENU_RW      220
#define SS_MENU_C0      460
#define SS_MENU_BH      190
#define SS_MENU_COL_LO  (SS_MENU_C0 - SS_MENU_BH + 1)   // 271
#define SS_MENU_COL_HI  (SS_MENU_C0 + 1)                // 461

// Layout menu rect (top-left quadrant in landscape)
#define LM_R0     50
#define LM_RW     145
#define LM_C0     350
#define LM_BH     110
#define LM_COL_LO (LM_C0 - LM_BH + 1)   // 241
#define LM_COL_HI (LM_C0 + 1)            // 351

// Scale menu rect (same quadrant as layout menu — never open simultaneously)
#define SM_R0     50
#define SM_RW     200
#define SM_C0     350
#define SM_BH     140
#define SM_COL_LO (SM_C0 - SM_BH + 1)   // 211
#define SM_COL_HI (SM_C0 + 1)            // 351

// Scale / display modes
#define SCALE_FILL  0   // stretch to fill 800x480 (default)
#define SCALE_FIT   1   // aspect-correct 533x480, 133 px black bars left/right
#define SCALE_3X    2   // integer 3x -> 480x432 centred with black border
#define SCALE_COUNT 3

// Rewind circular snapshot buffer
#define REWIND_SLOTS     40
#define REWIND_STATE_SZ  (96 * 1024)
#define REWIND_SNAP_FREQ 15

// ROM selector
#define ROMS_DIR "/sdcard/roms"
#define MAX_ROMS 64
