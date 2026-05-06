# HowBoyMatsu Development Log

## Project Overview
GBC emulator for Tanmatsu/ESP32-P4, branched from GnuBoy. Main source: `main/main.c`.

**Build:** `cd ~/HowBoyMatsu/HowBoyMatsu && make build DEVICE=tanmatsu`
**Upload:** `cd ~/Documents/HowBoyMatsu && sudo chmod 666 /dev/ttyACM1 && cd badgelink/tools && sudo ./badgelink.sh appfs upload application "HowBoyMatsu" 0 /home/irak4t0n/HowBoyMatsu/HowBoyMatsu/build/tanmatsu/application.bin`
**Monitor:** `cd ~/Documents/HowBoyMatsu && make monitor DEVICE=tanmatsu PORT=/dev/ttyACM0`

---

## Architecture

### Display
- Physical buffer: 480×800 (portrait), PAX rotated 90° CW to landscape
- PAX logical (x,y) → physical `buf[x * 480 + (479 - y)]`
- Game screen: 160×144 GBC → 800×480 via H_SCALE=3, V_SCALE=5
- Double-buffered: `render_buf_a`, `render_buf_b` in PSRAM

### Tasks
- **Core 0:** `blit_task`, `audio_task`, `ss_io_task`
- **Core 1:** `emulator_task` (runs `emu_run()` → gnuboy)
- Key semaphores: `sem_frame_ready/done`, `sem_audio_ready/done`, `sem_ss`, `sem_emulator_done`

### Audio Pipeline
- `pcm_submit()`: copies gnuboy audio to double buffer, gives `sem_audio_ready`
- `audio_task`: waits on `sem_audio_ready`, writes to I2S, gives `sem_audio_done`
- `audio_mute` flag: makes `pcm_submit` discard immediately (used during ROM selector)
- `i2s_enabled` flag: tracks I2S channel state to prevent double-enable/disable errors
- Codec: ES8156 via I2C, amplifier controlled via `bsp_audio_set_amplifier()`

### App Main Loop
`app_main` runs a `while(1)` loop supporting return-to-ROM-selector without hardware restart:
```c
while (1) {
    sem_emulator_done = xSemaphoreCreateBinary();
    // reset state flags, resume blit task, re-enable I2S if needed
    xTaskCreatePinnedToCore(emulator_task, ...);
    xSemaphoreTake(sem_emulator_done, portMAX_DELAY); // blocks until emulator exits
    if (!return_to_selector) break;
    // reset audio state, loop back to ROM selector
}
```

---

## Features Implemented

### 1. Save States ✅
- **F4** opens overlay menu (game keeps running)
- 10 slots per game at `/sdcard/saves/<romname>.ssN`
- Background IO task `ss_io_task` on Core 0
- Menu drawn directly onto live frame (~760µs, ~58fps during menu)

### 2. Fast Forward ✅
- **F6** cycles: OFF → 5× → 8× → OFF
- Frame skipping in `vid_end()`: `ff_skip[] = {0, 4, 7}`
- Audio muted during FF via `i2s_channel_disable()` on FF start, re-enabled on FF end
- `ff_silence_sent` counter prevents audio task from submitting during FF

### 3. Rewind (Time Rewind) ✅
- **F5** starts rewind; **F5 again** resumes from rewound point
- 40-slot circular snapshot buffer in PSRAM: `rewind_state_buf` (40 × 96 KB) + `rewind_pix_buf` (40 × 90 KB) — ~10 seconds at 59 FPS
- Snapshots taken every 15 frames via `fmemopen`/`savestate`; played back every 3 frames via `loadstate`
- **SRAM protected:** a copy of `ram.sbank` is saved to `rewind_sram_backup` before rewind starts; restored on exit — in-game saves are never corrupted
- All 8 GBC buttons force-released on rewind exit to prevent phantom movement
- Audio muted during rewind; buffer trimmed to resume point on exit

### 4. ROM Selector ✅
Full 800×480 landscape layout with direct pixel rendering (bypasses PAX rotation):
- `rom_fill_row_direct()` — memset rows directly into physical pixel buffer (~12ms for all rows)
- `rom_draw_text_direct()` — embedded 5×7 bitmap font, direct pixel write (~2ms for all text)
- Total full redraw: ~14ms (vs 575ms with PAX)
- Partial redraw on keypress: only 2 rows redrawn (~2ms)
- **PAX coordinate mapping:** logical (x,y) → physical `buf[x * 480 + (479 - y)]`

Layout: 60px header, 32px rows, 36px footer
Footer hint: `[Up/Down] Navigate   [Enter/A] Launch   [ESC] Exit`

### 4. Return to ROM Selector ✅ (Backspace)
Press **Backspace** during gameplay to save SRAM/RTC and return to ROM selector.

**Shutdown sequence (keyboard handler):**
1. `bsp_audio_set_amplifier(false)` + `bsp_audio_set_volume(0)` — mute immediately
2. `audio_mute = 1` — `pcm_submit` discards all new audio
3. `return_to_selector = 1` — signals `vid_end` to exit

**`vid_end` exit sequence:**
1. Suspend `blit_task`
2. Memset render buffers to black, blit black frame
3. Disable I2S if enabled
4. `xSemaphoreGive(sem_emulator_done)` — unblocks `app_main`
5. `vTaskDelete(NULL)` — kills emulator task

**New ROM start (`emulator_task`):**
1. Clear render buffers (prevents old frame flash)
2. Reset audio semaphores
3. `audio_mute = 0`
4. Re-enable I2S if disabled
5. `bsp_audio_set_volume(gbc_volume)` + `bsp_audio_set_amplifier(true)`
6. `emu_run()`

**SD card:** Only mounted once (`static int sd_mounted`), skipped on subsequent runs via `goto sd_already_mounted`.

---

## Key Globals
```c
static volatile int  return_to_selector = 0;
static volatile int  i2s_enabled = 1;
static volatile int  audio_mute = 0;
static volatile int  ff_speed = 0;
static volatile int  ff_silence_sent = 0;
static TaskHandle_t  blit_task_handle = NULL;
static TaskHandle_t  audio_task_handle = NULL;
static TaskHandle_t  emulator_task_handle = NULL;
static SemaphoreHandle_t sem_emulator_done = NULL;
static SemaphoreHandle_t sem_audio_shutdown = NULL;
```

---

## Button Mapping

### Default Layout (Layout 0)
| Key | Action |
|-----|--------|
| D-pad | GBC D-pad |
| A/a | GBC A |
| D/d | GBC B |
| Enter | Start |
| Space | Select |
| ` (backtick) | Toggle FPS |
| ESC | Save & exit to launcher |
| F1 | Soft reset (return to game title screen) |
| F2 | Button layout menu |
| F4 | Save state menu |
| F5 | Rewind (F5 again to resume) |
| F6 | Fast forward (OFF/5×/8×) |
| Backspace | Return to ROM selector |

### WASD Layout (Layout 1)
| Key | Action |
|-----|--------|
| D-pad | GBC D-pad |
| W/A/S/D | GBC D-pad (WASD) |
| ; | GBC A |
| [ | GBC B |
| Enter | Start |
| Space | Select |
| ` (backtick) | Toggle FPS |
| ESC | Save & exit to launcher |
| F1 | Soft reset (return to game title screen) |
| F2 | Button layout menu |
| F4 | Save state menu |
| F5 | Rewind (F5 again to resume) |
| F6 | Fast forward (OFF/5×/8×) |
| Backspace | Return to ROM selector |

---

## Planned Features
1. ~~Button Config Swap~~ — DONE (F2 menu, Default / WASD)
2. Return to Main Menu (ESC exits to launcher)
3. ~~Reverse Gameplay (rewind)~~ — DONE (F5, SRAM-safe)
4. Internal Resolution Scaling
5. ~~Texture Filtering/Shaders~~ — Removed: hardware bottlenecks (PSRAM write speed, RISC-V multiply cost) make smooth interpolation impossible at 59 FPS on ESP32-P4
6. Overclocking
7. Netplay
8. Input Mapping Profiles

---

## Session May 6 2026 — Launcher Icon Fix

### Fix: Off-centre screen element in launcher icon

The 32×32 launcher icon had the game console screen shifted ~3.5 px left of the body centre.

**Root cause:** Body spans cols 3–28 (centre = 15.5). Screen bezel was `fill(3, 15, 5, 20, S)` → cols 5–19 (centre = 12.0) and screen green was `fill(4, 14, 6, 19, G)` → cols 6–18 (centre = 12.0).

**Fix in `make_icon.py`:**
- Screen bezel: `fill(3, 15, 5, 20, S)` → `fill(3, 15, 9, 23, S)` — cols 9–22, centre 15.5
- Screen green: `fill(4, 14, 6, 19, G)` → `fill(4, 14, 10, 22, G)` — cols 10–21, centre 15.5

`icon.png` regenerated and re-uploaded to `/int/apps/application/icon.png` on the badge.

### Files Changed
- `make_icon.py` — screen bezel and green fill coordinates corrected
- `icon.png` — regenerated

---

## Session May 5 2026 (session 5)

### Feature: Scale Mode Menu (F3)

F3 opens a "SCALE" overlay menu with three display modes:

- **STRETCH** (0) — fills 800×480, 5×H / 3.33×V non-integer Y scale (original behaviour)
- **FIT** (1) — aspect-correct 533×480, 133 px black bars left/right; both axes scale at 480/144 = 3.33×; GBC X column count uses same 3+4 pattern as FILL Y scale to reach 533 rows
- **3X** (2) — integer 3×3 scale: 480×432 centred in 800×480 with 160 px rows above/below and 24 px col bands left/right; zero fractional pixels; pixels are square on-screen

**Implementation:** blit paths for all three modes were already present in `draw_gbc_screen()`. This session wired the UI:
- `draw_scale_menu()` — same overlay pattern as `draw_layout_menu()`
- F3 key handler — opens menu when no other menu is open
- Scale menu input routing — Up/Down cursor, Enter to confirm, F3 to cancel
- Scale menu overlay in `blit_task` — persistent-render pattern (scale_drawn_a/b)
- `scale_border_dirty = 2` on mode change — clears black border rows in both render buffers over 2 frames

---

## Session May 5 2026 (session 4)

### Extended Rewind Duration

Increased rewind buffer from ~3.4 seconds to ~10 seconds:

- `REWIND_SLOTS`: 20 → 40
- `REWIND_SNAP_FREQ`: 10 → 15 frames per snapshot (~0.25s granularity at 59 FPS)
- PSRAM cost: ~2.8 MB → ~5.6 MB (state buf + pixel buf)
- Allocation is guarded — if PSRAM is insufficient the rewind feature disables itself cleanly

---

## Session May 5 2026 (session 3)

### Removed: SMOOTH Display Mode

Attempted bilinear / smooth interpolation as an F3-selectable display mode. All approaches failed to meet the 59 FPS bar:

- **Row-major bilinear (per-pixel PSRAM writes):** 7–11 FPS — PSRAM per-pixel store latency (~50–80 cycles each) vs. memcpy burst
- **Column-major bilinear-Y (5× fewer lerps):** 38 FPS — 76,800 multiplications × ~40 cycles = ~12 ms/frame extra; 15 ms PSRAM floor + 12 ms compute = 27 ms → 37 FPS ceiling
- **Seam-blend (no multiply, 50/50 average with 0xF7DE mask):** 57–59 FPS but visually imperceptible at gameplay speeds — only 1 of 3–4 physical pixels per GBC row boundary was blended

Root cause: the 3.33× Y scale means each GBC source pixel spans 3–4 physical pixels. Any per-pixel math beyond memcpy either hits the PSRAM write bottleneck or the multiply cost; the seam-only approach avoids both but is too subtle to see.

**Decision:** removed the entire SMOOTH mode and F3 shader menu. F3 is now unassigned.

---

## Session May 5 2026 (session 2)

### Feature: Display Mode / Shader Menu (F3)

**New key:** F3 opens a "DISPLAY" menu overlay with cursor-selectable display modes.
Modes:
- **NEAREST** (0) — original nearest-neighbor block scaling (pixel-perfect)
- **HD SHARP** (1) — sharp bilinear interpolation

**Sharp bilinear algorithm:**
- Precomputed LUT (`bilinear_lut_init`, called lazily on first HD frame):
  - `blit_gx0/gx1/tx[800]` — physical row → GBC X neighbours + sharpened fraction
  - `blit_gy0/gy1/ty[480]` — physical col → GBC Y neighbours + sharpened fraction (inverted for 90° rotation)
- Sharp curve: `clamp(0.5 + (t - 0.5) * 4, 0, 1)` — pixel interiors map to 0 or 1, only ±1/8 of pixel width does actual blending → crisp not blurry
- Fixed-point two-step bilinear: lerp X (>>8), lerp Y (>>8), no floats in inner loop
- Writes all 800×480 output pixels; skips preserved menu rects same as nearest path

**Globals added:** `shader_mode`, `shader_menu_open`, `shader_cursor`, `shader_drawn_a/b`, `shader_invalidate()`
**Defines added:** `SHADER_NEAREST`, `SHADER_SHARP`, `SHADER_COUNT`, `SM_R0/RW/C0/BH/COL_LO/HI`

---

## Session May 5 2026

### Key Remapping
Reorganized function key assignments for better ergonomics:
- **Backtick (`)** — Toggle FPS counter (was ESC)
- **ESC** — Save & return to launcher (was F1)
- **F1** — Soft reset / return to game title screen (was F3)
- **F3** — Unassigned (freed up)

All affected locations updated: gameplay navigation handler, ROM selector handler, error/no-ROM
wait loops, ROM selector footer hint text, error screen prompt text.

### UI Color Theme — Red
Replaced all green/blue UI accents with red throughout the ROM selector and menus:
- All RGB565 green (`0x07E0`) → red (`0xF800`): menu borders, cursors, separators, FPS overlay
- Selected row (`0x07F1`) → dark red (`0x8000`)
- PAX mint green (`0xFF00FF88`) → PAX red (`0xFFFF0000`)
- ROM selector header/footer panel navy (`0xFF1A1A2E`) → dark maroon (`0xFF2E1A1A`)
- ROM selector alternating row colors (`0x180C`, `0x2104`) → red shades (`0x1800`, `0x2000`)
- HowBoyMatsu title text green (`0xFF00FF00`) → red (`0xFFFF0000`)

### Save State — DELETE option
Added a fourth menu item **DELETE** to the save state overlay (op code 3 in `ss_io_task`):
- Calls `remove()` on the `.ssN` file path
- Clears `ss_exists[slot]`; shows "Slot N deleted!" toast
- DELETE shown in red when slot exists, greyed out when slot is empty

### Rewind — SRAM Protection
Rewound state snapshots use `savestate()`/`loadstate()` which overwrites `ram.sbank`. Fixed by:
- Copying `ram.sbank` to `rewind_sram_backup` (128 KB PSRAM) on F5 entry
- Restoring on F5-exit and on auto-exhaust; setting `ram.sram_dirty = 1`

### Rewind — Flash Fix
Set `rw_frame_ctr = 2` on rewind entry so the first `vid_end` call immediately pops a snapshot
rather than displaying 2 stale frames.

---

## Session Apr 27 2026

### Fix: Double HowBoyMatsu directory

The repo had been cloned into `~/HowBoyMatsu/HowBoyMatsu`. Cleaned up by removing the
nested copy and pulling fresh from GitHub. All commands now use `~/HowBoyMatsu` as root.

### Commands Reference (updated)

**Build:**
```bash
cd ~/HowBoyMatsu && make build DEVICE=tanmatsu
```

**Build + Upload (one liner):**
```bash
cd ~/HowBoyMatsu && make build DEVICE=tanmatsu && sudo chmod 666 /dev/ttyACM1 && cd badgelink/tools && sudo ./badgelink.sh appfs upload application "HowBoyMatsu" 0 ~/HowBoyMatsu/build/tanmatsu/application.bin
```

**Monitor:**
```bash
cd ~/HowBoyMatsu && make monitor DEVICE=tanmatsu PORT=/dev/ttyACM0
```

### Fix: F1 Return to Launcher

**Problem:** F1 in-game was returning to the ROM selector instead of the Tanmatsu launcher.

**Root cause:** `bsp_device_restart_to_launcher()` in BSP v0.9.3 is a broken stub for
IDF 5.5 — it only calls `esp_restart()` without clearing the appfs boot selection. The
appfs bootloader sees the retained boot handle and relaunches the app automatically.

**Fix:** Added `restart_to_launcher()` helper that clears `mem->custom` (zeroing the
appfs bootsel magic) then calls `esp_restart()`. Replaced all `bsp_device_restart_to_launcher()` calls.

```c
#include "bootloader_common.h"

static void restart_to_launcher(void) {
    rtc_retain_mem_t* mem = bootloader_common_get_rtc_retain_mem();
    memset(mem->custom, 0, sizeof(mem->custom));
    esp_restart();
}
```

### Planned Features (status)

- [x] Button Config Swap — partially explored, reverted. Clean rework next session.
- [x] Return to Main Menu — FIXED (F1 now correctly returns to Tanmatsu launcher)
- [ ] Reverse Gameplay (Rewind)
- [ ] Internal Resolution Scaling
- [ ] Texture Filtering / Shaders
- [ ] Overclocking
- [ ] Netplay
- [ ] Input Mapping Profiles

### Notes for Next Session

**Button Layout Switcher** — two presets, F2 cycles between them:
- Layout 1 (Default): D-pad=directions, a=A, d=B, Enter=Start, Space=Select
- Layout 2 (WASD): w/a/s/d=directions, l=A, p=B, Enter=Start, Space=Select

---

## Session May 3 2026

### Switched to Windows + Native ESP-IDF

Old environment was Linux + Makefile-driven build pinned to ESP-IDF v5.5.1.
New environment is Windows + native ESP-IDF PowerShell, no Makefile (its
`SHELL := /usr/bin/env bash` and `source export.sh` directives are bash-only).

**Critical:** the project must be built against **ESP-IDF v5.5.1 specifically**.
v5.5.4 (the version the Espressif Windows installer offered by default) builds
clean but the resulting binary blue-screens on launch — likely a managed-component
ABI drift (`badge-bsp 0.9.5` vs the 0.9.3 the Apr 27 build pinned, plus other
component bumps). Cloning v5.5.1 alongside and building against it fixes the
crash:

```powershell
git clone -b v5.5.1 --recursive --depth 1 https://github.com/espressif/esp-idf.git C:\Users\Howar\esp-idf-5.5.1
cd C:\Users\Howar\esp-idf-5.5.1; .\install.ps1 esp32p4
```

Each new shell needs `. C:\Users\Howar\esp-idf-5.5.1\export.ps1` before `idf.py`.

**Build (Windows-equivalent of `make build DEVICE=tanmatsu`):**

```powershell
idf.py -B build/tanmatsu build -DDEVICE=tanmatsu -DSDKCONFIG_DEFAULTS="sdkconfigs/general;sdkconfigs/tanmatsu" -DSDKCONFIG=sdkconfig_tanmatsu -DIDF_TARGET=esp32p4
```

### Tanmatsu USB Layout (Windows)

The Tanmatsu exposes **two separate USB-CDC interfaces**, one per chip:
- **COM15** = ESP32-P4 main chip (HowBoyMatsu console output, `idf.py monitor`)
- **COM16** = ESP32-C6 coprocessor (radio firmware `tanmatsu-radio`)

When the launcher is in **"USB / Badgelink mode"** (purple-diamond key → USB icon
top-right), it re-enumerates a separate composite device with the legacy Badge.team
WebUSB descriptor **VID `0x16D0` PID `0x0F9A`** — that's the device badgelink
talks to. WCID-bound to Windows' built-in WinUSB driver automatically; no Zadig
needed. Just need libusb-1.0.dll (x86_64) dropped in `badgelink/tools/libraries/`.

`idf.py monitor` on Windows needs `$env:PYTHONIOENCODING="utf-8"` and
`[Console]::OutputEncoding = [System.Text.Encoding]::UTF8` set first, otherwise
the launcher's USB descriptor table (printed with Unicode box-drawing characters)
crashes the monitor with a `cp1252 codec can't encode ┌` error.

### Save State Menu — Background Panel + Persistent-Render Architecture

**Goal:** the save state menu was hard to read against bright/busy game scenes.
Add a dark background panel + green border behind the menu text.

**v1 (works visually, breaks audio):** Filled a 220×210 dark navy panel + 4
border lines + text in `draw_ss_menu` every frame. Visually correct, but:
- Menu draw cost: ~1.78 ms per frame
- FPS dropped 60 → 56 while menu open
- Audio crackled — buffer is exactly 1 frame (1470 samples @ 44.1kHz = 16.67 ms),
  so any frame slowdown causes I2S underrun

Profiling showed PSRAM-bandwidth-bound (writing 92KB/frame at the chip's
~52MB/s ceiling = ~1.78 ms — no CPU-side optimization will help).

**v2 (architectural fix):** Don't redraw the menu every frame.
- New `SS_MENU_R0/RW/C0/BH` constants — single source of truth for the rect
- New `ss_menu_drawn_a` / `ss_menu_drawn_b` per-buffer flags
- `vid_end` skips writing the menu rect when `ss_state` is open (game render
  splits each row into two memcpys around the menu rect — adds ~165 µs/frame)
- `blit_task` only calls `draw_ss_menu` when the current buffer's `drawn` flag
  is 0 (just-opened or invalidated) or while a toast is animating
- `ss_menu_invalidate()` called at every state change (open, cursor move, slot
  change, save/load completion)

Result: menu draws **once per buffer per state change** (~4 draws per open/close
cycle) instead of ~60/sec. FPS during menu-open returned to 60. Audio crackle
greatly reduced.

**Known issue:** Slight residual audio distortion remains even with FPS at 60
during menu open. Suspected causes (not yet investigated):
1. Per-row memcpy split in `vid_end` adds jitter inside the tight 16.67 ms
   audio buffer window
2. EMI/electrical: solid dark menu pixels change LCD power draw pattern,
   could couple into the ES8156 audio rail
3. PSRAM bus contention between blit and audio task

Tolerable for now. Possible next-session diagnostic: revert the `vid_end` skip
(keep the dirty-flag system) to isolate which sub-cause it is.

### `ss_rect` Optimization (kept, didn't help)

Replaced the `for (int i = 0; i < ch; i++) row[i] = col;` inner loop with
32-bit pixel-pair writes (one `uint32_t` = 2 packed `uint16_t` pixels):

```c
uint32_t col32 = ((uint32_t)col << 16) | col;
for (int i = 0; i < ch >> 1; i++) row32[i] = col32;
```

Halves the number of PSRAM transactions but didn't measurably reduce draw time —
PSRAM bandwidth, not CPU, is the bottleneck. Kept the change anyway because
it's strictly better and the architectural fix made the per-frame cost moot.

### Files Changed
- `main/main.c` — all of the above
- `README.md` — note the new background panel under Save States
- `.gitignore` — `.claude/` (Claude Code session state)

### Planned Features (status)
- [x] Save State Menu Visibility — FIXED (dark panel + persistent-render)
- [x] Button Layout Switcher — FIXED (F2 menu, Default / WASD)
- [x] Soft Reset — FIXED (F3 resets game to title screen, SRAM preserved)
- [ ] Reverse Gameplay (Rewind)
- [ ] Internal Resolution Scaling
- [ ] Texture Filtering / Shaders
- [ ] Overclocking
- [ ] Netplay
- [ ] Input Mapping Profiles
- [ ] Investigate residual audio distortion when save state menu is open

---

## Session May 3 2026 (continued)

### Button Layout Switcher (F2)

**Goal:** Add an on-the-fly button layout switcher so the user can switch between Default
and WASD key mappings without restarting.

**Design:**
- **F2** opens a small layout menu (top-left area of the game screen)
- Up/Down arrow or Enter/A to select; F2 or Enter to confirm
- Active layout shown in green, cursor shown with `>`
- Two layouts:

| Layout | D-pad | A button | B button |
|--------|-------|----------|----------|
| Default | Navigation keys | a/A key | d/D key |
| WASD | w/a/s/d keys (+ nav keys still work) | ; key | [ key |

**Architecture:**

Same persistent-render pattern as the save state menu:
- `layout_menu_open` flag signals `vid_end` to skip the menu rect rows (preserves pixels across frames)
- `lm_drawn_a` / `lm_drawn_b` per-buffer dirty flags: `blit_task` only calls `draw_layout_menu` when the buffer is stale
- `lm_invalidate()` called on open, cursor move, and confirm

**Key expansion:**

`key_release_time[]` and `key_pads[]` expanded from 4 → 8 entries:
- Indices 0–3: A, B, Start, Select (both layouts)
- Indices 4–7: Up, Down, Left, Right (WASD d-pad, released when switching back to Default)

When switching layouts (Enter confirms), all WASD-mapped d-pad inputs are force-released
(`pad_set(PAD_UP/DOWN/LEFT/RIGHT, 0)` + `key_release_time[4..7] = 0`) to prevent stuck inputs.

**Menu position:** R0=50, RW=145, C0=350, BH=110 (physical coords) — top-left quadrant in
landscape, doesn't overlap save state menu (rows 560–779).

### Files Changed
- `main/main.c` — all of the above

---

## Session May 3 2026 (continued — ROM reload flash fix)

### Fix: Old ROM Audio/Video Flash on ROM Reload

**Problem:** After pressing Backspace to return to the ROM selector and launching a
different ROM, there was a brief window where the old ROM's last audio frame played
and its last video frame was visible before the new ROM started.

**Root causes identified:**

1. **`pcm_init()` spawned a duplicate `audio_task` on every ROM load** — the function
   had no idempotency guard, so each call to `emu_run()` created a new audio task
   while the old one kept running. Two audio tasks writing to I2S simultaneously caused
   the audio glitch.

2. **`pcm_init()` also leaked memory on every reload** — three `malloc()` calls for
   `pcm.buf`, `audio_buf_a`, `audio_buf_b` and two `xSemaphoreCreateBinary()` calls
   with no corresponding `free` / `vSemaphoreDelete`. Globals were silently overwritten.

3. **`gbc_pixels` never cleared between ROMs** — the static pixel buffer retains the
   previous ROM's last rendered scanline until the new ROM overwrites it. On fast ROM
   loads, `vid_end` could blit stale pixels before the first emulated frame filled the buffer.

**Fix:**

`pcm_init()` made idempotent with a `static int inited` guard:
- First call: allocates buffers, creates semaphores, spawns `audio_task` — same as before
- Subsequent calls: zero all three audio buffers, reset `pcm.pos`, return immediately
  (audio task keeps running uninterrupted across ROM reloads)

`memset(gbc_pixels, 0, sizeof(gbc_pixels))` added just before `emu_run()` in
`emulator_task` — guarantees first blitted frame is black rather than stale VRAM.

### Files Changed
- `main/main.c` — `pcm_init()` idempotency guard, `gbc_pixels` clear before `emu_run()`

---

## Session May 3 2026 (continued — F3 soft reset)

### Feature: Soft Reset (F3)

**Goal:** Press F3 during gameplay to reset the current game back to its title screen
without exiting to the ROM selector or launcher.

**Implementation:**

`BSP_INPUT_NAVIGATION_KEY_F3` handler added to the main navigation switch in `doevents()`,
guarded so it only fires when both the save state menu and layout menu are closed:

```c
case BSP_INPUT_NAVIGATION_KEY_F3:
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
```

`emu_reset()` (in `components/gnuboy/emu.c`) calls `hw_reset()`, `lcd_reset()`,
`cpu_reset()`, `mbc_reset()`, `sound_reset()` — full hardware power-up state.
SRAM is not touched, so in-game saves survive the reset.

The dirty-flag calls (`vram_dirty`, `pal_dirty`, `sound_dirty`, `mem_updatemap`)
match the post-SRAM-load sequence already used at ROM start — they ensure the
renderer picks up the freshly reset hardware state on the next frame.

`pcm_init()` is called to zero the audio buffers for a clean audio start (it is
idempotent and does not spawn a new audio task).

`gbc_pixels` is cleared first so there is no single-frame flash of the old game state.

### Files Changed
- `main/main.c` — F3 soft reset handler
- `README.md` — F3 added to button mapping, feature bullet, backlog entry marked done
- `DEVLOG.md` — this entry

---

## Session May 4 2026 — Fast Forward audio mute fix

### Fix: FF audio distortion on repeated use

**Problem:** First FF cycle muted audio correctly. On subsequent cycles, audio played
through distorted instead of muting.

**Root cause:** `ff_silence_sent` (a counter used to flush the I2S DMA with 8 silence
frames on FF start) was never reset between cycles. On the second FF activation it was
already at 8, so the flush was skipped and the DMA replayed its last stale audio buffer.

**Fix (attempt 1 — reset counter):** Reset `ff_silence_sent = 0` in both F6 handlers
whenever `ff_speed > 0`. Confirmed working for mute.

**Attempted improvement — play audio at normal speed during FF:**
Replaced silence-flush with a stride approach: submit 1 out of every N audio frames
(N=5 at 5x, N=8 at 8x) so the audio task receives exactly 60 buffers/second regardless
of FF speed. Sounded bad in practice — the emulator produces frames in bursts between
blits, causing uneven buffer delivery and audible gaps/stutters.

**Final fix — non-blocking silence feed:** During FF, `pcm_submit()` attempts
`xSemaphoreTake(sem_audio_done, 0)` (zero timeout, non-blocking). If the audio task
is ready, write a silence buffer and give `sem_audio_ready`. If not, discard and move on.
Result: clean mute with no emulator slowdown, no DMA replay, works on every FF cycle.

### Files Changed
- `main/main.c` — `pcm_submit()` FF block rewritten; `ff_silence_sent` → non-blocking silence feed

---

## Session May 5 2026 — Rewind Gameplay (F5) ✅

### Feature: Rewind Gameplay (F5)

**Goal:** Press F5 to rewind gameplay in real time; press F5 again to resume from the rewound point. In-game saves (SRAM) must not be corrupted.

### Design

**Circular snapshot buffer (PSRAM):**
- `rewind_state_buf`: 20 × 96 KB = 1.92 MB of gnuboy savestates
- `rewind_pix_buf`: 20 × 160×144 × 2B = ~900 KB of pixel snapshots
- `rewind_sram_backup`: 128 KB — copy of `ram.sbank` taken at rewind entry

**Snapshot cadence:** `rewind_push()` called every 10 frames during 1× normal play (not during FF). Each call writes `savestate()` + pixel data to the next circular slot.

**Playback cadence:** Every 3 frames, `rewind_pop()` loads the previous snapshot via `loadstate()` + `vram_dirty/pal_dirty/sound_dirty/mem_updatemap`. Between pops, `rw_hold_pixels` is re-blitted to hold the frame steady.

**SRAM protection (key fix):** gnuboy's `savestate()`/`loadstate()` include `ram.sbank` (battery-backed SRAM, i.e. in-game saves). Without protection, rewinding past an in-game save would roll that save back in memory, and the next autosave would corrupt the `.sav` file.

Fix: on F5 entry, `memcpy(rewind_sram_backup, ram.sbank, mbc.ramsize * 8192)`. On both F5-exit and auto-exhaust, restore it back and set `ram.sram_dirty = 1`. The player's saves are fully preserved regardless of how far they rewind.

**Both exit paths (F5 and buffer-exhausted auto-exit):**
1. Trim `rw_head`/`rw_count` to the resume point
2. Release all 8 GBC buttons (`pad_set` to 0) to prevent phantom movement
3. Restore `ram.sbank` from backup, set `sram_dirty`
4. Unmute audio

**Key implementation details:**
- `rw_frame_ctr` is file-scope (not static-local) and reset to 0 on every rewind entry — ensures clean 3-frame cadence on 2nd+ uses
- Snapshots not taken during fast-forward (`ff_speed == 0 && !rewind_active` guard)
- `sdkconfig` entries for `CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC` retained (needed for launcher-exit function)

### Files Changed
- `main/main.c` — rewind feature (globals, `rewind_push`, `rewind_pop`, `rewind_release_all_keys`, F5 handler, `vid_end` rewind block + snapshot push, PSRAM allocation)
- `DEVLOG.md` — this entry; Features, Button Mapping, Planned Features updated
- `README.md` — F5 added to button mapping and features list; backlog entry marked done
