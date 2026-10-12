/* commands.h: the vendor command channel (feature report 2). MIT licence.
 * A command is `opcode, length, payload` in the 63 bytes after the report ID. Steam sends one with
 * SET_REPORT and reads its answer with GET_REPORT. */
#pragma once
#include <stdint.h>

/* `iface` is the USB interface: 0 to 3 for the slots, 4 for the dock. `body` is 63 bytes. */
int commands_set(int iface, const uint8_t *body);
int commands_get(int iface, uint8_t *body);

/* Reboot into the UF2 bootloader (what Steam's updater asks for) or plain reboot, after 20 ms. */
void reboot_to_bootloader(void);
void reboot_normally(void);
