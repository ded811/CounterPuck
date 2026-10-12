/* shell.c: a few diagnostic commands on the USB serial port. MIT licence. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include "bonds.h"
#include "channels.h"
#include "commands.h"
#include "identity.h"
#include "slot.h"
#include "usb.h"

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "serial %s, board serial %s, revision %s, build time 0x%08X, board revision 0x%02X",
		    identity_serial(), identity_board_serial(), identity_revision(),
		    identity_build_time(), identity_board_revision());
	return 0;
}

static int cmd_bonds(const struct shell *sh, size_t argc, char **argv)
{
	for (int i = 0; i < NUM_SLOTS; i++) {
		const struct bond *b = bond_get(i);

		shell_print(sh, "slot %d: puck ID 0x%08X, controller ID 0x%08X, controller %.16s", i,
			    b->puck_id, b->controller_id, b->controller_serial);
	}
	return 0;
}

static int cmd_connection(const struct shell *sh, size_t argc, char **argv)
{
	for (int i = 0; i < NUM_SLOTS; i++) {
		uint8_t ch[4];
		uint32_t up;

		slot_info(i, ch, &up);
		shell_print(sh, "slot %d: %s", i, slot_state_name(i));
		if (slot_connected(i)) {
			shell_print(sh, "  connected for %u s, channels %u %u %u %u", up, ch[0], ch[1], ch[2],
				    ch[3]);
		}
	}
	return 0;
}

/* pair <slot> [seconds] */
static int cmd_pair(const struct shell *sh, size_t argc, char **argv)
{
	int slot = atoi(argv[1]);
	int seconds = argc > 2 ? CLAMP(atoi(argv[2]), 1, 255) : 60;

	if (slot < 0 || slot >= NUM_SLOTS) {
		shell_error(sh, "slot must be 0 to %d", NUM_SLOTS - 1);
		return -EINVAL;
	}
	slot_start_pairing(slot, seconds);
	shell_print(sh, "slot %d: pairing window opened", slot);
	return 0;
}

/* connection_qos [interval_ms] [1 to print each report] */
static int cmd_qos(const struct shell *sh, size_t argc, char **argv)
{
	uint16_t interval = argc > 1 ? strtoul(argv[1], NULL, 10) : 500;
	bool print = argc > 2 && strtoul(argv[2], NULL, 10) != 0;

	for (int i = 0; i < NUM_SLOTS; i++) {
		slot_set_qos(i, interval, print);
	}
	shell_print(sh, "link quality reports every %u ms, printing %s", interval, print ? "on" : "off");
	return 0;
}

static int cmd_factory_restore(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2 || strcmp(argv[1], "--really") != 0) {
		shell_print(sh, "This forgets every pairing. Run: factory_restore --really");
		return 0;
	}
	bonds_clear_all();
	reboot_normally();
	return 0;
}

static int cmd_bootloader(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "Rebooting into the UF2 bootloader: copy a .uf2 onto the drive that appears");
	reboot_to_bootloader();
	return 0;
}

static int cmd_records(const struct shell *sh, size_t argc, char **argv)
{
	for (int id = 0; id < 0x80; id++) {
		if (slot_input_count(id)) {
			shell_print(sh, "input report 0x%02X: %u records", id, slot_input_count(id));
		}
	}
	return 0;
}

static int cmd_trace_output(const struct shell *sh, size_t argc, char **argv)
{
	bool on = argc < 2 || strcmp(argv[1], "off") != 0;

	slot_trace_output(on);
	shell_print(sh, "output report tracing %s", on ? "on" : "off");
	return 0;
}

SHELL_CMD_REGISTER(records, NULL, "Count the input report records controllers have sent", cmd_records);
SHELL_CMD_REGISTER(info, NULL, "Serial numbers and build identity", cmd_info);
SHELL_CMD_REGISTER(bootloader, NULL, "Reboot into the UF2 bootloader (to reflash)", cmd_bootloader);
SHELL_CMD_REGISTER(trace_output, NULL, "trace_output [off]: log output reports sent to controllers", cmd_trace_output);
SHELL_CMD_REGISTER(bonds, NULL, "Show the pairing records", cmd_bonds);
SHELL_CMD_REGISTER(connection, NULL, "Show the state of each slot", cmd_connection);
SHELL_CMD_ARG_REGISTER(pair, NULL, "pair <slot> [seconds]: open a pairing window", cmd_pair, 2, 1);
SHELL_CMD_REGISTER(connection_qos, NULL, "connection_qos [interval_ms] [1]: link quality reports",
		   cmd_qos);
SHELL_CMD_REGISTER(factory_restore, NULL, "factory_restore --really: forget every pairing",
		   cmd_factory_restore);
