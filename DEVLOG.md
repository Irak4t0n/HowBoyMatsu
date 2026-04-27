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
