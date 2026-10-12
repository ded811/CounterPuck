/* main.c: start-up order. MIT licence. */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include "bonds.h"
#include "channels.h"
#include "identity.h"
#include "radio.h"
#include "slot.h"
#include "usb.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
	int err;

	identity_init();
	err = settings_subsys_init();
	if (err) {
		LOG_ERR("The settings store failed to start (%d): pairings will not be kept", err);
	} else {
		bonds_load();
	}

	/* The slots must exist before the USB device is enabled: the host's first requests already
	 * reach them. */
	slots_init();
	channels_init();
	err = usb_start();
	if (err) {
		return err;
	}
	LOG_INF("USB started");
	err = radio_start();
	if (err) {
		return err;
	}
	LOG_INF("Radio started");
	channels_start_scan();

	LOG_INF("Starting Counterpuck %s, build time 0x%08X", identity_revision(),
		identity_build_time());
	slots_start();
	return 0;
}
