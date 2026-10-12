/* combiner.c: half-reports 0x60 / 0x61 -> report 0x45. MIT licence. */
#include <string.h>
#include <zephyr/kernel.h>
#include "combiner.h"
#include "hid_desc.h"
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

struct state {
	struct report45 r;
	int64_t last_ms;
	bool active;
};
static struct state st[NUM_SLOTS];

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

static void send(int slot)
{
	struct state *s = &st[slot];
	uint8_t out[sizeof(struct report45)] __aligned(4);

	s->r.seq++;
	memcpy(out, &s->r, sizeof(out));
	(void)usb_submit_input(slot, out, sizeof(out));
}

void combiner_half(int slot, const uint8_t *rec)
{
	struct state *s = &st[slot];
	struct half h;

	memcpy(&h, rec, sizeof(h));
	s->r.id = RPT_COMBINED;
	if (h.id == 0x60) {
		s->r.axis_a = widen(h.axis);
		memcpy(s->r.a01, &h.v[0], 4);
		memcpy(s->r.a27, &h.v[2], 12);
		s->r.buttons = apply_buttons(s->r.buttons, h.buttons, map_60);
	} else if (h.id == 0x61) {
		s->r.axis_b = widen(h.axis);
		memcpy(s->r.b01, &h.v[0], 4);
		s->r.buttons = apply_buttons(s->r.buttons, h.buttons, map_61);
	} else {
		return;
	}
	s->active = true;
	s->last_ms = k_uptime_get();
	send(slot);
}

void combiner_tick(int slot)
{
	struct state *s = &st[slot];

	if (s->active && k_uptime_get() - s->last_ms >= 1000) {
		/* Nothing for a second: a neutral report, everything but the ID and sequence cleared. */
		uint8_t seq = s->r.seq;

		memset(&s->r, 0, sizeof(s->r));
		s->r.seq = seq;
		s->r.id = RPT_COMBINED;
		s->last_ms = k_uptime_get();
		send(slot);
	}
}

void combiner_reset(int slot)
{
	memset(&st[slot], 0, sizeof(st[slot]));
}
