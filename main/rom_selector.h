#pragma once

#include "config.h"

extern char rom_list[MAX_ROMS][300];
extern int  rom_count;

void        scan_roms(void);
const char *rom_selector(void);
