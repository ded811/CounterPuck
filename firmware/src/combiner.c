/* combiner.c: half-reports 0x60 / 0x61 -> report 0x45. MIT licence. */
#include <string.h>
#include <zephyr/kernel.h>
#include "combiner.h"
#include "hid_desc.h"
#include "slot.h"
#include "usb.h"

struct __packed half {
	uint8_t id;
	uint16_t buttons;
	uint8_t axis;
	int16_t v[8];
};

struct __packed report45 {
	uint8_t id;
	uint8_t seq;
	uint32_t buttons;
	int16_t axis_a;          /* from 0x60 */
	int16_t axis_b;          /* from 0x61 */
	int16_t a01[2];          /* 0x60 v[0..1] */
	int16_t b01[2];          /* 0x61 v[0..1] */
	int16_t unused[8];       /* never written */
	int16_t a27[6];          /* 0x60 v[2..7] */
};
BUILD_ASSERT(sizeof(struct half) == 20);
BUILD_ASSERT(sizeof(struct report45) == 46);

/* Which bit of the 32-bit button field each of a half's 12 button bits sets (docs/wire-formats.md). */
static const uint8_t map_60[12] = { 10, 11, 12, 13, 17, 19, 15, 27, 16, 14, 24, 29 };
static const uint8_t map_61[12] = { 0, 1, 2, 3, 7, 9, 5, 23, 4, 6, 20, 28 };

static struct report45 merged;          /* the one combined state */
static int64_t last_half_ms;
static bool serving[NUM_SLOTS];         /* this slot's controller sends half-reports */
static K_MUTEX_DEFINE(lock);

/* The lowest-numbered connected slot that sends half-reports, or -1. */
static int target(void)
{
	for (int i = 0; i < NUM_SLOTS; i++) {
		if (serving[i] && slot_connected(i)) {
			return i;
		}
	}
	return -1;
}

/* A byte widened to 15 bits: 0x00 -> 0, 0x80 -> 0x407F, 0xFF -> 0x7FFF. */
static int16_t widen(uint8_t v)
{
	return (int16_t)((v << 7) | ((v & 0x80) ? 0x7F : 0));
}

static uint32_t apply_buttons(uint32_t field, uint16_t src, const uint8_t *map)
{
	for (int i = 0; i < 12; i++) {
		field = (src & BIT(i)) ? (field | BIT(map[i])) : (field & ~BIT(map[i]));
	}
	return field;
}

/* Send the merged state to `slot` (caller holds the lock). */
static void send(int slot)
{
	uint8_t out[sizeof(struct report45)] __aligned(4);

	merged.seq++;
	memcpy(out, &merged, sizeof(out));
	(void)usb_submit_input(slot, out, sizeof(out));
}

static void make_neutral(void)
{
	/* everything but the ID and the sequence number cleared */
	uint8_t seq = merged.seq;

	memset(&merged, 0, sizeof(merged));
	merged.seq = seq;
	merged.id = RPT_COMBINED;
}

void combiner_half(int slot, const uint8_t *rec)
{
	struct half h;
	int to;

	memcpy(&h, rec, sizeof(h));
	k_mutex_lock(&lock, K_FOREVER);
	merged.id = RPT_COMBINED;
	if (h.id == 0x60) {
		merged.axis_a = widen(h.axis);
		memcpy(merged.a01, &h.v[0], 4);
		memcpy(merged.a27, &h.v[2], 12);
		merged.buttons = apply_buttons(merged.buttons, h.buttons, map_60);
	} else if (h.id == 0x61) {
		merged.axis_b = widen(h.axis);
		memcpy(merged.b01, &h.v[0], 4);
		merged.buttons = apply_buttons(merged.buttons, h.buttons, map_61);
	} else {
		k_mutex_unlock(&lock);
		return;
	}
	serving[slot] = true;
	last_half_ms = k_uptime_get();
	to = target();
	if (to >= 0) {
		send(to);
	}
	k_mutex_unlock(&lock);
}

void combiner_tick(int slot)
{
	int to;

	k_mutex_lock(&lock, K_FOREVER);
	/* Only the served slot's thread acts, so one second of silence gives one neutral report. */
	if (serving[slot] && target() == slot && k_uptime_get() - last_half_ms >= 1000) {
		make_neutral();
		last_half_ms = k_uptime_get();
		to = target();
		if (to >= 0) {
			send(to);
		}
	}
	k_mutex_unlock(&lock);
}

void combiner_release(int slot)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (serving[slot]) {
		serving[slot] = false;
		/* The controller is gone: let go of everything it held, on its own interface. */
		make_neutral();
		send(slot);
		if (target() < 0) {
			memset(&merged, 0, sizeof(merged));   /* nobody left: start from nothing next time */
		}
	}
	k_mutex_unlock(&lock);
}
