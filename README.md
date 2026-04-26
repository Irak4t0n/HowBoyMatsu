# HowBoyMatsu
A Game Boy Color emulator for the Tanmatsu handheld/Konsool, derived from the GnuBoy project.

*Last updated: April 26, 2026*

---

## Features
- **~60 FPS emulation** — dual-core rendering pipeline (emulator on Core 1, blit on Core 0)
- **Full screen display** — stretched to fill the 800×480 display in landscape orientation
- **Stereo audio** — via ES8156 DAC at 44100Hz with dedicated audio task
- **ROM selector** — navigate with D-pad, launch with A or Enter
- **SRAM save/load** — in-game saves persist across sessions
- **RTC save/load** — real-time clock state preserved (for Pokémon Gold/Silver/Crystal)
- **Autosave** — SRAM saved automatically every 5 minutes
- **Save States** — 10 save slots per game, save/load full emulator state to SD card (F4)
- **Fast Forward** — 5× and 8× speed modes with audio muted during FF (F6)
- **FPS counter** — toggle with ESC key, displayed top-right in green
- **Clean launcher exit** — press F1 to save and return to the Tanmatsu launcher

## Button Mapping
| Tanmatsu | Action |
|----------|--------|
| D-pad | D-pad |
| A / a key | Game Boy A |
| D / d key | Game Boy B |
| Enter | Start |
| Space | Select |
| Volume Up/Down | Volume |
| ESC | Toggle FPS counter |
| F1 | Save & return to launcher |
| F4 | Save State menu (10 slots) |
| F6 | Fast Forward (OFF → 5× → 8× → OFF) |

## Save States
Press **F4** to open the save state menu. The game continues running in the background.
- **D-pad Up/Down** — navigate menu items
- **D-pad Left/Right** — cycle through slots 0–9
- **A** — confirm (Save/Load/Cancel)
- **B or F4** — close menu

Save files are stored at `/sdcard/saves/<romname>.ssN`.

## Fast Forward
Press **F6** to cycle through speed modes:
- **OFF** — normal 60 FPS with audio
- **5×** — ~160 FPS, audio muted
- **8×** — ~200 FPS, audio muted

## ROM Setup
Place `.gb` and `.gbc` ROM files in `/sdcard/roms/` on your SD card. Save files are stored automatically in `/sdcard/saves/`. Both original Game Boy (DMG) and Game Boy Color ROMs are supported.

## Known Limitations
- Slight audio distortion inherent to gnuboy's sound synthesis engine
- Brief turquoise flash when returning to the launcher (launcher initialization)
- Audio is muted during fast forward

---

## Planned Features
### 🗓️ Backlog
| # | Feature | Notes |
|---|---------|-------|
| 1 | **Button Config Swap** | Swap button layouts on the fly |
| 2 | **Return to ROM Launcher** | Jump back to the ROM selector without full restart |
| 3 | **Return to Main Menu** | Return to the Tanmatsu main launcher menu |
| 4 | **Reverse Gameplay** | Circular frame buffer rewind — memory intensive |
| 5 | **Internal Resolution Scaling** | Dynamic `SCALE` factor beyond 1:1 |
| 6 | **Texture Filtering / Shaders** | Post-process pass on PAX framebuffer |
| 7 | **Overclocking** | ESP32-P4 CPU freq tuning via `esp_pm` |
| 8 | **Netplay** | WiFi link cable emulation |
| 9 | **Input Mapping Profiles** | Multiple button layout presets stored in NVS |

---

## Notes
- Tested with Pokémon Gold, Pokémon Crystal, Super Mario Bros. Deluxe, Metal Gear Solid, Legend of Zelda: Link's Awakening
- Built with ESP-IDF v5.5.1 for ESP32-P4

---

*Please don't hesitate to reach out with any advice or questions.*

*-Irak4t0n- (The pseudonym my Flipper gave me)*
*-KeleXBrimbor- (Everywhere else)*
