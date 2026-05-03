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

### 3. ROM Selector ✅
Full 800×480 landscape layout with direct pixel rendering (bypasses PAX rotation):
- `rom_fill_row_direct()` — memset rows directly into physical pixel buffer (~12ms for all rows)
- `rom_draw_text_direct()` — embedded 5×7 bitmap font, direct pixel write (~2ms for all text)
- Total full redraw: ~14ms (vs 575ms with PAX)
- Partial redraw on keypress: only 2 rows redrawn (~2ms)
- **PAX coordinate mapping:** logical (x,y) → physical `buf[x * 480 + (479 - y)]`

Layout: 60px header (green `#00FF88`), 32px rows, 36px footer
Footer hint: `[Up/Down] Select  [Left/Right] Page  [Enter/A] Launch  [F1] Exit`

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
| Key | Action |
|-----|--------|
| D-pad | GBC D-pad |
| A/a | GBC A |
| D/d | GBC B |
| Enter | Start |
| Space | Select |
| ESC | Toggle FPS |
| F1 | Save & exit to launcher |
| F4 | Save state menu |
| F6 | Fast forward (OFF/5×/8×) |
| Backspace | Return to ROM selector |

---

## Planned Features
1. Button Config Swap
2. Return to Main Menu (F1 already exits to launcher)
3. Reverse Gameplay (rewind)
4. Internal Resolution Scaling
5. Texture Filtering/Shaders
6. Overclocking
7. Netplay
8. Input Mapping Profiles

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
- [ ] Reverse Gameplay (Rewind)
- [ ] Internal Resolution Scaling
- [ ] Texture Filtering / Shaders
- [ ] Overclocking
- [ ] Netplay
- [ ] Input Mapping Profiles
- [ ] Button Layout Switcher (carried over from Apr 27 session)
- [ ] Investigate residual audio distortion when save state menu is open
