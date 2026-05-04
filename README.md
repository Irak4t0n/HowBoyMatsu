# HowBoyMatsu
A Game Boy Color emulator for the Tanmatsu handheld/Konsool, derived from the GnuBoy project.

*Last updated: May 3, 2026*

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
- **Return to ROM selector** — press Backspace during gameplay to save and return to the ROM selector without a hardware restart
- **Button layout switcher** — press F2 to open a layout menu; choose Default (a/d) or WASD (w/a/s/d + ;/[)
- **Soft reset** — press F3 to reset the current game back to its title screen (SRAM saves preserved)

## Button Mapping

### Default Layout
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
| F2 | Button layout menu |
| F3 | Soft reset (return to game title screen) |
| F4 | Save State menu (10 slots) |
| F6 | Fast Forward (OFF → 5× → 8× → OFF) |
| Backspace | Return to ROM selector |

### WASD Layout (press F2 to switch)
| Tanmatsu | Action |
|----------|--------|
| D-pad | D-pad (still active) |
| W / A / S / D | D-pad Up / Left / Down / Right |
| ; key | Game Boy A |
| [ key | Game Boy B |
| Enter / Space / F1 / F3 / F4 / F6 | Same as Default |

## Save States
Press **F4** to open the save state menu. The game continues running in the background.
- **D-pad Up/Down** — navigate menu items
- **D-pad Left/Right** — cycle through slots 0–9
- **A** — confirm (Save/Load/Cancel)
- **B or F4** — close menu

The menu renders on a dark navy background panel with a green border so it stays
readable on top of any game scene. The panel is drawn once when the menu opens
(and again on cursor / slot changes) — the game blit skips the menu rect on
subsequent frames so it persists without per-frame redraw cost.

Save files are stored at `/sdcard/saves/<romname>.ssN`.

## ROM Selector
The ROM selector displays all `.gb` and `.gbc` files found in `/sdcard/roms/`. Navigation is instant — the selector uses a direct pixel renderer bypassing PAX rotation, achieving full redraws in ~14ms.

- **D-pad Up/Down** — move selection
- **A or Enter** — launch ROM
- **F1** — save and return to launcher

Press **Backspace** at any time during gameplay to save SRAM/RTC and return to the ROM selector without a hardware restart.

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
| 1 | ~~**Button Config Swap**~~ | ✅ Done — F2 menu: Default / WASD |
| 2 | ~~**Soft Reset**~~ | ✅ Done — F3 resets game to title screen (SRAM preserved) |
| 3 | **Reverse Gameplay** | Circular frame buffer rewind — memory intensive |
| 4 | **Internal Resolution Scaling** | Dynamic `SCALE` factor beyond 1:1 |
| 5 | **Texture Filtering / Shaders** | Post-process pass on PAX framebuffer |
| 6 | **Overclocking** | ESP32-P4 CPU freq tuning via `esp_pm` |
| 7 | **Netplay** | WiFi link cable emulation |

---

## Notes
- Tested with Pokémon Gold, Pokémon Crystal, Super Mario Bros. Deluxe, Metal Gear Solid, Legend of Zelda: Link's Awakening
- Built with ESP-IDF v5.5.1 for ESP32-P4

---

*Please don't hesitate to reach out with any advice or questions.*

*-Irak4t0n- (The pseudonym my Flipper gave me)*
*-KeleXBrimbor- (Everywhere else)*
