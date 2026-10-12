/* bonds.c: pairing records in the settings store, keys "puck/bond0" to "puck/bond3". MIT licence. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include "bonds.h"
#include "usb.h"

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
	if (!bond_valid(b)) {
		memset(&records[slot], 0, sizeof(records[slot]));
		return settings_delete(key);
	}
	err = settings_save_one(key, b, sizeof(*b));
	if (err == 0) {
		records[slot] = *b;
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
