/* commands.c: the vendor commands Steam sends to the puck. MIT licence. */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>
#include "commands.h"
#include "bonds.h"
#include "channels.h"
#include "hid_desc.h"
#include "identity.h"
#include "slot.h"
#include "usb.h"
#include "puck_config.h"

LOG_MODULE_REGISTER(commands, LOG_LEVEL_INF);

#define BOOTLOADER_MAGIC_REBOOT  0xECAABAC0u   /* payload of 0x95 that means "bootloader" */
#define FACTORY_RESET_MAGIC      0xEFC1A917u   /* payload of 0x86 that confirms it */

/* The last command set on each interface, so that the read that follows knows what to answer. */
static uint8_t last_cmd[NUM_IFACES][FEATURE_BODY_LEN + 1];

typedef int (*set_fn)(int iface, const uint8_t *payload, uint8_t len);
typedef int (*get_fn)(int iface, uint8_t *data, uint8_t *len);   /* data = request payload in, answer out */

struct command {
	uint8_t opcode;
	set_fn set;
	get_fn get;
};

/* ---- reboots ------------------------------------------------------------------------------ */

static void do_bootloader(struct k_work *w)
{
	/* The Adafruit UF2 bootloader enters its drive mode when this magic is left in GPREGRET. */
	nrf_power_gpregret_set(NRF_POWER, 0, 0x57);
	sys_reboot(SYS_REBOOT_COLD);
}
static void do_reboot(struct k_work *w) { sys_reboot(SYS_REBOOT_COLD); }
static K_WORK_DELAYABLE_DEFINE(bootloader_work, do_bootloader);
static K_WORK_DELAYABLE_DEFINE(reboot_work, do_reboot);

/* 20 ms later, so the USB request that asked for it can finish first. */
void reboot_to_bootloader(void) { k_work_schedule(&bootloader_work, K_MSEC(20)); }
void reboot_normally(void) { k_work_schedule(&reboot_work, K_MSEC(20)); }

static void do_factory_reset(struct k_work *w)
{
	bonds_clear_all();
	k_msleep(100);
	sys_reboot(SYS_REBOOT_COLD);
}
static K_WORK_DELAYABLE_DEFINE(factory_work, do_factory_reset);

/* ---- answers ------------------------------------------------------------------------------ */

static int get_attributes(int iface, uint8_t *data, uint8_t *len)
{
	const struct { uint8_t tag; uint32_t value; } __packed attr[5] = {
		{ 1, PUCK_USB_PID },
		{ 2, 0 },                                 /* capabilities */
		{ 10, 0 },                                /* bootloader build time: not known here */
		{ 4, identity_build_time() },
		{ 9, identity_board_revision() },
	};

	memcpy(data, attr, sizeof(attr));
	*len = sizeof(attr);
	return 0;
}

static int get_string(int iface, uint8_t *data, uint8_t *len)
{
	const char *text;
	uint8_t tag = data[0];

	switch (tag) {
	case 0: case 4: text = identity_board_serial(); break;
	case 1:         text = identity_serial(); break;
	case 3:         text = identity_revision(); break;
	default:        text = "NA"; break;
	}
	memset(data + 1, 0, 19);
	strncpy((char *)data + 1, text, 19);   /* the tag, then the text, 20 bytes in all */
	*len = 20;
	return 0;
}

static int get_wireless_state(int iface, uint8_t *data, uint8_t *len)
{
	data[0] = (iface < NUM_SLOTS && slot_connected(iface)) ? 2 : 1;
	*len = 1;
	return 0;
}

static int get_device_info(int iface, uint8_t *data, uint8_t *len)
{
	uint8_t selector = data[0];
	uint32_t word;

	*len = 0;
	if (selector == 0) {
		memset(data + 1, 0, 40);
		word = identity_build_time();
		memcpy(data + 1, &word, 4);
		word = identity_board_revision();
		memcpy(data + 5, &word, 4);
		strncpy((char *)data + 9, identity_revision(), 16);
		strncpy((char *)data + 25, identity_serial(), 16);
		*len = 41;
	} else if (selector == 1) {
		memset(data + 1, 0, 33);               /* no crash has ever been recorded */
		*len = 34;
	}
	return 0;
}

static int get_channel_stats(int iface, uint8_t *data, uint8_t *len)
{
	uint8_t mode = data[0];

	for (int i = 0, ch = 0; ch < 80; ch += 2, i++) {
		uint8_t v;

		switch (mode) {
		case 0:  v = ch; break;
		case 1:  v = iface < NUM_SLOTS ? channels_error_percent(iface, ch) : 0xFF; break;
		case 2:  v = channels_noise(ch); break;
		case 3:  v = channels_noise_peak(ch); break;
		default: v = 0xFF; break;
		}
		data[1 + i] = v;
	}
	*len = 41;
	return 0;
}

static int get_bond(int iface, uint8_t *data, uint8_t *len)
{
	if (iface < NUM_SLOTS) {
		memcpy(data, bond_get(iface), sizeof(struct bond));
		*len = sizeof(struct bond);
	} else {
		*len = 0;                              /* the dock has no pairing */
	}
	return 0;
}

/* ---- actions ------------------------------------------------------------------------------ */

static int set_bond(int iface, const uint8_t *payload, uint8_t len)
{
	struct bond b;

	if (iface >= NUM_SLOTS) {
		return 0;
	}
	memcpy(&b, payload, sizeof(b));
	slot_set_bond(iface, &b);
	return 0;
}

static int set_pairing(int iface, const uint8_t *payload, uint8_t len)
{
	if (iface >= NUM_SLOTS) {
		return 0;
	}
	if (payload[0] == 0) {
		slot_cancel_pairing(iface);
	} else {
		slot_start_pairing(iface, payload[1] ? payload[1] : PUCK_PAIRING_SECONDS);
	}
	return 0;
}

static int set_dock_input(int iface, const uint8_t *payload, uint8_t len)
{
	return -ENOTSUP;   /* no dock on this board */
}

static int set_factory_reset(int iface, const uint8_t *payload, uint8_t len)
{
	uint32_t magic;

	memcpy(&magic, payload, 4);
	if (magic == FACTORY_RESET_MAGIC) {
		LOG_INF("Factory reset");
		k_work_schedule(&factory_work, K_MSEC(10));
	} else {
		LOG_INF("Ignoring factory reset without the confirmation code");
	}
	return 0;
}

static int bootloader_request(void)
{
#if PUCK_STEAM_MAY_REBOOT_TO_BOOTLOADER
	reboot_to_bootloader();
	return 0;
#else
	LOG_WRN("Ignoring a request to reboot into the bootloader (PUCK_STEAM_MAY_REBOOT_TO_BOOTLOADER is 0)");
	return -ENOTSUP;
#endif
}

static int set_reboot(int iface, const uint8_t *payload, uint8_t len)
{
	uint32_t magic;

	memcpy(&magic, payload, 4);
	if (magic == BOOTLOADER_MAGIC_REBOOT) {
		return bootloader_request();
	} else {
		reboot_normally();
	}
	return 0;
}

static int set_reboot_to_bootloader(int iface, const uint8_t *payload, uint8_t len)
{
	return bootloader_request();
}

static const struct command table[] = {
	{ 0xB4, NULL,                       get_wireless_state },
	{ 0x83, NULL,                       get_attributes },
	{ 0xAE, NULL,                       get_string },
	{ 0xF2, NULL,                       get_device_info },
	{ 0xF1, set_dock_input,             NULL },
	{ 0xAD, set_pairing,                NULL },
	{ 0xF3, NULL,                       get_channel_stats },
	{ 0xA3, NULL,                       get_bond },
	{ 0x86, set_factory_reset,          NULL },
	{ 0x95, set_reboot,                 NULL },
	{ 0x90, set_reboot_to_bootloader,   NULL },
	{ 0xA2, set_bond,                   NULL },
};

static const struct command *find(uint8_t opcode)
{
	for (size_t i = 0; i < ARRAY_SIZE(table); i++) {
		if (table[i].opcode == opcode) {
			return &table[i];
		}
	}
	return NULL;
}

int commands_set(int iface, const uint8_t *body)
{
	unsigned int total = (unsigned int)body[1] + 2;   /* opcode, length, payload */
	const struct command *c = find(body[0]);

	if (!c || total > FEATURE_BODY_LEN) {
		return -EINVAL;
	}
	if (c->set) {
		int err = c->set(iface, body + 2, body[1]);

		if (err) {
			return err;
		}
	}
	memset(last_cmd[iface], 0, sizeof(last_cmd[iface]));
	memcpy(last_cmd[iface], body, total);
	return 0;
}

int commands_get(int iface, uint8_t *body)
{
	const uint8_t *last = last_cmd[iface];
	const struct command *c = find(last[0]);
	int err;

	if (!c) {
		return -EINVAL;
	}
	if (!c->get) {
		return -EOPNOTSUPP;
	}
	memset(body, 0, FEATURE_BODY_LEN);
	memcpy(body, last, last[1] + 2);          /* the request, so the handler can read its payload */
	err = c->get(iface, body + 2, &body[1]);
	return err < 0 ? err : 0;
}
