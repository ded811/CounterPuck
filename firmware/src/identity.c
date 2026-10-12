/* identity.c: serial numbers and build identity. MIT licence. */
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include "identity.h"
#include "puck_config.h"

BUILD_ASSERT(sizeof(PUCK_SERIAL) == 14, "PUCK_SERIAL must be exactly 13 characters");
BUILD_ASSERT(sizeof(PUCK_BOARD_SERIAL) == 14, "PUCK_BOARD_SERIAL must be exactly 13 characters");
BUILD_ASSERT(sizeof(PUCK_GIT_REVISION) <= 16, "PUCK_GIT_REVISION must be at most 15 characters");

static char serial[14] = PUCK_SERIAL;
static char board_serial[14] = PUCK_BOARD_SERIAL;

void identity_init(void)
{
#if PUCK_SERIAL_FROM_CHIP
	uint8_t id[8] = { 0 };

	if (hwinfo_get_device_id(id, sizeof(id)) > 0) {
		snprintf(serial, sizeof(serial), "CPK%02X%02X%02X%02X%02X",
			 id[0], id[1], id[2], id[3], id[4]);
		snprintf(board_serial, sizeof(board_serial), "CPB%02X%02X%02X%02X%02X",
			 id[7], id[6], id[5], id[4], id[3]);
	}
#endif
}

const char *identity_serial(void) { return serial; }
const char *identity_board_serial(void) { return board_serial; }
const char *identity_revision(void) { return PUCK_GIT_REVISION; }
uint32_t identity_build_time(void) { return PUCK_BUILD_TIME; }
uint32_t identity_board_revision(void) { return PUCK_BOARD_REVISION; }
