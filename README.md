# HowBoyMatsu
A Game Boy Color emulator for the Tanmatsu handheld/Konsool, derived from the GnuBoy project.
*Last updated: April 25, 2026*

---

## Features
- **~60 FPS emulation** — dual-core rendering pipeline (emulator on Core 1, blit on Core 0)
- **Full screen display** — stretched to fill the 800×480 display in landscape orientation
- **Stereo audio** — via ES8156 DAC at 44100Hz with dedicated audio task
- **ROM selector** — navigate with D-pad, launch with A or Enter
- **SRAM save/load** — in-game saves persist across sessions
- **RTC save/load** — real-time clock state preserved (for Pokémon Gold/Silver/Crystal)
- **Autosave** — SRAM saved automatically every 5 minutes
- **FPS counter** — toggle with ESC key, displayed top-right in green
- **Clean launcher exit** — press F1 to save and return to the Tanmatsu launcher

## Button Mapping
| Tanmatsu | Game Boy |
|----------|----------|
| D-pad | D-pad |
| A / a key | A |
| D / d key | B |
| Enter | Start |
| Space | Select |
| Volume Up/Down | Volume |
| ESC | Toggle FPS counter |
| F1 | Save & return to launcher |

## ROM Setup
Place `.gb` and `.gbc` ROM files in `/sdcard/roms/` on your SD card. Save files are stored automatically in `/sdcard/saves/`. Both original Game Boy (DMG) and Game Boy Color ROMs are supported.

## Known Limitations
- Slight audio distortion inherent to gnuboy's sound synthesis engine
- Brief turquoise flash when returning to the launcher (launcher initialization)

---

## Planned Features

### 🔜 In Progress

| # | Feature | Notes |
|---|---------|-------|
| 1 | **Save State** | Slot-based save/load of full emulator state to SD card — `savestate()`/`loadstate()` core exists, wiring up SD path, button combo trigger, and on-screen toast |
| 2 | **Button Config Swap** | Swap button layouts on the fly with a button combo |
| 3 | **Return to ROM Launcher** | Jump back to the ROM selector without full restart |
| 4 | **Return to Main Menu** | Return to the Tanmatsu main launcher menu |
| 5 | **Fast Forward** | Speed up emulation (2×/4×) while holding a button |

### 🗓️ Backlog

| # | Feature | Notes |
|---|---------|-------|
| 6 | **Reverse Gameplay** | Circular frame buffer rewind — memory intensive |
| 7 | **Internal Resolution Scaling** | Dynamic `SCALE` factor beyond 1:1 |
| 8 | **Texture Filtering / Shaders** | Post-process pass on PAX framebuffer |
| 9 | **Overclocking** | ESP32-P4 CPU freq tuning via `esp_pm` |
| 10 | **Netplay** | WiFi link cable emulation |
| 11 | **Input Mapping Profiles** | Multiple button layout presets stored in NVS |

---

## Notes
- Tested with Pokémon Gold, Pokémon Crystal
- Built with ESP-IDF v5.5.1 for ESP32-P4

---

*Please don't hesitate to reach out with any advice or questions.*

*-Irak4t0n- (The pseudonym my Flipper gave me)*

*-KeleXBrimbor- (Everywhere else)*
