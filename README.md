# HowBoyMatsu
A Game Boy Color emulator for the Tanmatsu handheld/Konsool, derived from the GnuBoy project.

*Last updated: May 6, 2026 (session 7)*

---

## Features
- **~60 FPS emulation** — dual-core rendering pipeline (emulator on Core 1, blit on Core 0)
- **Full screen display** — stretched to fill the 800×480 display in landscape orientation
- **Stereo audio** — via ES8156 DAC at 44100Hz with dedicated audio task
- **ROM selector** — navigate with D-pad, launch with A or Enter
- **SRAM save/load** — in-game saves persist across sessions
- **RTC save/load** — real-time clock state preserved (for Pokémon Gold/Silver/Crystal)
- **Autosave** — SRAM saved automatically every 5 minutes
- **Scale modes** — F3 menu: STRETCH (fills screen), FIT (aspect-correct 533×480), 3X (integer 3× pixel-perfect centered)
- **Save States** — 10 save slots per game, save/load full emulator state to SD card (F4)
- **Rewind** — press F5 to rewind gameplay in real time; press F5 again to resume from the rewound point. In-game saves (SRAM) are fully protected — no save corruption
- **Fast Forward** — 5× and 8× speed modes with audio muted during FF (F6)
- **FPS counter** — toggle with ESC key, displayed top-right in green
- **Clean launcher exit** — press F1 to save and return to the Tanmatsu launcher
- **Return to ROM selector** — press Backspace during gameplay to save and return to the ROM selector without a hardware restart
- **Button layout switcher** — press F2 to open a layout menu; choose Default (a/d) or WASD (w/a/s/d + ;/[)
- **Soft reset** — press F1 to reset the current game back to its title screen (SRAM saves preserved)

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
| ` (backtick) | Toggle FPS counter |
| ESC | Save & return to launcher |
| F1 | Soft reset (return to game title screen) |
| F2 | Button layout menu |
| F3 | Scale mode menu (STRETCH / FIT / 3X) |
| F4 | Save State menu (10 slots) |
| F5 | Rewind (press again to resume) |
| F6 | Fast Forward (OFF → 5× → 8× → OFF) |
| Backspace | Return to ROM selector |

### WASD Layout (press F2 to switch)
| Tanmatsu | Action |
|----------|--------|
| D-pad | D-pad (still active) |
| W / A / S / D | D-pad Up / Left / Down / Right |
| ; key | Game Boy A |
| [ key | Game Boy B |
| Enter / Space / ESC / F1 / F2 / F3 / F4 / F5 / F6 | Same as Default |

## Scale Modes
Press **F3** to open the scale menu:
- **STRETCH** — stretches the game to fill the full 800×480 display (default, non-integer Y scale)
- **FIT** — aspect-correct 533×480, 133 px black bars left and right, same proportions as the original GBC screen
- **3X** — integer 3× scale, 480×432 pixel-perfect centered image with black borders on all sides

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

## Rewind
Press **F5** to start rewinding gameplay. The last ~10 seconds of play (40 snapshots, one every 15 frames) is stored in PSRAM. Press **F5 again** to resume from the rewound point.

- Audio is muted during rewind
- In-game saves (SRAM) are fully protected — a backup is taken when rewind starts and restored on exit, so no save data can be corrupted regardless of how far you rewind

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
| 2 | ~~**Soft Reset**~~ | ✅ Done — F1 resets game to title screen (SRAM preserved) |
| 3 | ~~**Reverse Gameplay**~~ | ✅ Done — F5 rewind, SRAM-safe |
| 4 | ~~**Internal Resolution Scaling**~~ | ✅ Done — F3 menu: STRETCH / FIT / 3X |
| 5 | ~~**Texture Filtering / Shaders**~~ | Removed: PSRAM write bottleneck + RISC-V multiply cost make smooth interpolation impossible at 59 FPS |
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
