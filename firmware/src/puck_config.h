/* puck_config.h: every setting you might want to change, in one place.
 * MIT licence. Each setting says what it is for and what a real Steam Controller 2 puck uses. */
#pragma once

/* ---- Identity -------------------------------------------------------------------------------- */

/* Unit serial: exactly 13 characters, A-Z and 0-9. It is the USB serial number and the string Steam
 * reads as "unit serial". A real puck has a factory serial of this shape (3 letters, then digits).
 * Leave PUCK_SERIAL_FROM_CHIP on to make one from the chip's unique ID ("CPK" + 10 hex digits), so
 * two boards never clash. Turn it off and set PUCK_SERIAL to choose your own. */
#define PUCK_SERIAL_FROM_CHIP 1
#define PUCK_SERIAL           "CPK0000000000"

/* Board (PCBA) serial: same shape, reported separately. A real puck has a different one. */
#define PUCK_BOARD_SERIAL     "CPB0000000000"

/* Board revision reported to Steam. A real puck reports 0x47. Other values change what Steam
 * thinks it is talking to, so only change this on purpose. */
#define PUCK_BOARD_REVISION   0x47

/* ---- USB ------------------------------------------------------------------------------------- */

/* A real puck is a Valve device: vendor 0x28DE, product 0x1304. Steam finds a puck by these IDs, so
 * they are needed for it to be recognised at all. */
#define PUCK_USB_VID          0x28DE
#define PUCK_USB_PID          0x1304
#define PUCK_USB_MANUFACTURER "Valve Software"
#define PUCK_USB_PRODUCT      "Steam Controller Puck"

/* ---- Build identity ---------------------------------------------------------------------------- */

/* Build time (Unix seconds, UTC) and a short revision string, reported to Steam. Steam's updater
 * compares the build time with the one it ships to decide whether to offer an update. Valve's
 * current puck build is 0x6A628359. Use a NEWER value than that so Steam never offers to replace
 * this firmware with Valve's; use an older one only if you want it to. */
#define PUCK_BUILD_TIME       0x6A628361u
#define PUCK_GIT_REVISION     "cpuck-0.1.2"   /* at most 15 characters */

/* ---- Steam updates ------------------------------------------------------------------------ */

/* Steam can ask a puck to reboot into its bootloader (vendor commands 0x90, and 0x95 with a code)
 * to update or downgrade its firmware. A real puck then talks Valve's update protocol. This
 * board's bootloader (UF2) does not, so Steam cannot update it; the request would only leave the
 * board waiting as a USB drive.
 *   0: ignore Steam's request, and say so in the log (the default). To reflash, use the shell
 *      command `bootloader`, or double-tap reset.
 *   1: obey it: the board reboots into the UF2 bootloader, as the request asks. */
#define PUCK_STEAM_MAY_REBOOT_TO_BOOTLOADER 0

/* ---- Pairing ----------------------------------------------------------------------------------- */

/* How long a pairing window stays open when Steam asks for the default (a real puck: 60 s). */
#define PUCK_PAIRING_SECONDS  60

/* ---- Radio ------------------------------------------------------------------------------------- */

/* Transmit power in dBm: 0, 2, 3, 4, 5, 6, 7 or 8. A real puck uses +6. */
#define PUCK_TX_POWER_DBM     6

/* The shell prompt ("dongle~$ ", as on a real puck) is set in prj.conf (CONFIG_SHELL_PROMPT_UART). */
