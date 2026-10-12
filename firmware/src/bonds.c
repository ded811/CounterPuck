/* bonds.c: pairing records in the settings store, keys "puck/bond0" to "puck/bond3". MIT licence. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include "bonds.h"
#include "radio.h"
#include <zephyr/logging/log.h>
#include "usb.h"

LOG_MODULE_REGISTER(bonds, LOG_LEVEL_INF);

static struct bond records[NUM_SLOTS];

bool bond_valid(const struct bond *b)
{
	return b->puck_id != 0 && b->controller_id != 0;
}

const struct bond *bond_get(int slot) { return &records[slot]; }

static int on_setting(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	for (int i = 0; i < NUM_SLOTS; i++) {
		char name[8];

		snprintf(name, sizeof(name), "bond%d", i);
		if (settings_name_steq(key, name, &next) && !next) {
			if (len != sizeof(struct bond)) {
				return -EINVAL;
			}
			ssize_t n = read_cb(cb_arg, &records[i], sizeof(struct bond));

			return n < 0 ? (int)n : 0;
		}
	}
	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(puck_bonds, "puck", NULL, on_setting, NULL, NULL);

int bonds_load(void)
{
	return settings_load_subtree("puck");
}

int bond_set(int slot, const struct bond *b)
{
	char key[16];
	int err;

	snprintf(key, sizeof(key), "puck/bond%d", slot);
	/* The record in memory is what counts for this session; the flash copy keeps it across reboots. */
	radio_hold();   /* a flash write stalls the CPU: no radio transaction may be in flight */
	if (!bond_valid(b)) {
		memset(&records[slot], 0, sizeof(records[slot]));
		err = settings_delete(key);
	} else {
		records[slot] = *b;
		err = settings_save_one(key, b, sizeof(*b));
	}
	radio_release();
	if (err) {
		LOG_ERR("Storing the pairing of slot %d failed (%d): it is lost when the puck restarts",
			slot, err);
	}
	return err;
}

int bonds_clear_all(void)
{
	struct bond none = { 0 };
	int err = 0;

	for (int i = 0; i < NUM_SLOTS; i++) {
		err |= bond_set(i, &none);
	}
	return err;
}
