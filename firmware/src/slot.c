/* slot.c: the radio slots. MIT licence.
 *
 * Every slot runs a small state machine in its own thread:
 *   IDLE       no pairing record; waits for a pairing request
 *   PAIRING    beacons on channel 2 until a controller in pairing mode answers
 *   CONNECTING asks the paired controller to come to a data channel
 *   CONNECTED  exchanges a packet every few ms; the controller's reports ride in the replies
 * The formats and the timing are in docs/wire-formats.md and docs/puck-internals.md. */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include "slot.h"
#include "bonds.h"
#include "channels.h"
#include "combiner.h"
#include "hid_desc.h"
#include "identity.h"
#include "radio.h"
#include "usb.h"
#include "puck_config.h"

LOG_MODULE_REGISTER(slot, LOG_LEVEL_INF);

enum state { IDLE, PAIRING, CONNECTING, CONNECTED };
static const char *const state_names[] = { "IDLE", "PAIRING", "CONNECTING", "CONNECTED" };

/* Events a slot thread waits for. */
#define EV_STOP           BIT(0)   /* the pairing record changed */
#define EV_PAIR           BIT(1)
#define EV_CANCEL         BIT(2)
#define EV_HOST_SUSPENDED BIT(3)
#define EV_HOST_RESUMED   BIT(4)
#define EV_TIMER          BIT(5)

/* Records in the packets (docs/wire-formats.md). */
#define REC_SET_FEATURE    1
#define REC_SET_RESULT     2
#define REC_GET_FEATURE    3
#define REC_GET_ANSWER     4
#define REC_OUTPUT_REPORT  5
#define REC_INPUT_REPORT   6

#define QUEUE_ITEM 66     /* {length, type, up to 64 bytes} */
#define QUEUE_DEPTH 20

struct slot {
	int index;
	int pipe;                         /* ESB pipe: index + 1 */
	struct k_thread thread;
	struct k_event events;
	struct k_timer timer;
	struct k_msgq queue;              /* records waiting to go to the controller */
	uint8_t queue_buf[QUEUE_ITEM * QUEUE_DEPTH];

	volatile enum state state;
	enum state last_logged;           /* so "idle" is logged once per visit */
	bool host_suspended;
	bool send_version, tell_host_asleep, tell_turn_off, wake_blocked;
	uint8_t protocol;                 /* the controller's protocol version */
	uint8_t pairing_seconds;
	uint8_t channel[4];               /* current channel, then alternates (0xFF = none) */
	int64_t connected_at_ms;
	uint32_t history;                 /* which of the last 11 exchanges needed retries */

	/* feature report 1 exchange with the controller */
	struct k_mutex fr_lock;
	struct k_timer fr_timer;
	uint8_t fr_state;                 /* 0 idle, 1 SET queued, 2 GET sent, 3 answer ready */
	uint8_t fr_opcode;
	uint8_t fr_answer[FEATURE_BODY_LEN];

	/* link quality window */
	int64_t window_start_ms;
	uint16_t ok, retries, max_gap_ms;
	int32_t rssi_sum;                 /* sum of dB below 0 dBm over the successful exchanges */
	int64_t last_ack_ms;
	uint16_t qos_interval_ms;
	bool qos_print;
};

static struct slot slots[NUM_SLOTS];
K_THREAD_STACK_ARRAY_DEFINE(slot_stacks, NUM_SLOTS, 3072);

static uint32_t wait_events(struct slot *s, k_timeout_t timeout)
{
	uint32_t got = k_event_wait(&s->events, 0xFFFFFFFFu, false, timeout);

	if (got) {
		k_event_clear(&s->events, got);
	}
	return got;
}

/* ---- reports to the PC ------------------------------------------------------------------- */

static void send_status(struct slot *s, uint8_t status)
{
	uint8_t r[2] = { RPT_STATUS, status };

	(void)usb_submit_input(s->index, r, sizeof(r));
}

static void release_keys_and_buttons(struct slot *s)
{
	uint8_t kb[1 + 8] = { RPT_KEYBOARD };
	uint8_t mouse[1 + 5] = { RPT_MOUSE };

	(void)usb_submit_input(s->index, kb, sizeof(kb));
	(void)usb_submit_input(s->index, mouse, sizeof(mouse));
}

/* The keyboard emulation may only press Enter, Escape, Tab and the arrow keys, and no modifiers. */
static bool keyboard_report_allowed(const uint8_t *r)
{
	if (r[1] != 0 || r[2] != 0) {
		return false;
	}
	for (int i = 3; i < 9; i++) {
		uint8_t key = r[i];

		if (key < 2 || key == 0x28 || key == 0x29 || key == 0x2B || (key >= 0x4F && key <= 0x52)) {
			continue;
		}
		return false;
	}
	return true;
}

/* ---- link quality ----------------------------------------------------------------------- */

static void window_reset(struct slot *s, int64_t now)
{
	s->window_start_ms = now;
	s->ok = s->retries = s->max_gap_ms = 0;
	s->rssi_sum = 0;
}

static void send_quality(struct slot *s, uint8_t reason)
{
	int64_t now = k_uptime_get();
	uint8_t r[13] = { RPT_QUALITY };
	uint16_t window = (uint16_t)MIN(now - s->window_start_ms, 0xFFFF);
	int8_t mean_dbm = s->ok ? -(int8_t)MIN(s->rssi_sum / s->ok, 127) : 0;

	sys_put_le16(window, &r[1]);
	sys_put_le16(s->ok, &r[3]);
	sys_put_le16(s->retries, &r[5]);
	sys_put_le16(s->max_gap_ms, &r[7]);
	r[9] = (uint8_t)mean_dbm;
	r[10] = reason;
	r[11] = s->channel[0];
	r[12] = s->channel[1];
	if (s->qos_print) {
		LOG_INF("Slot %d: QoS %u ms, ok %u, retries %u, gap %u ms, %d dBm, reason %u, ch %u/%u",
			s->index, window, s->ok, s->retries, s->max_gap_ms, mean_dbm, reason,
			r[11], r[12]);
	}
	(void)usb_submit_input(s->index, r, sizeof(r));
	window_reset(s, now);
}

/* ---- records from the controller -------------------------------------------------------- */

static void handle_record(struct slot *s, const uint8_t *rec)
{
	uint8_t len = rec[0], type = rec[1];
	const uint8_t *data = rec + 2;

	switch (type) {
	case REC_SET_RESULT:
		if (len == 4 && (int32_t)sys_get_le32(data) != 0) {
			LOG_WRN("Slot %d: the controller refused a feature report (%d)", s->index,
				(int)(int32_t)sys_get_le32(data));
		}
		break;
	case REC_GET_ANSWER:
		k_mutex_lock(&s->fr_lock, K_FOREVER);
		if (s->fr_state == 2) {
			memset(s->fr_answer, 0, sizeof(s->fr_answer));
			memcpy(s->fr_answer, data, MIN(len, FEATURE_BODY_LEN));
			s->fr_state = 3;
		}
		k_mutex_unlock(&s->fr_lock);
		break;
	case REC_INPUT_REPORT: {
		uint8_t id = data[0];

		if (id == 0x60 || id == 0x61) {
			if (len >= 20) {
				combiner_half(s->index, data);
			}
		} else if (id >= RPT_MOUSE && id <= RPT_COMBINED && hid_input_len(id) + 1 == len) {
			if (id != RPT_KEYBOARD || keyboard_report_allowed(data)) {
				(void)usb_submit_input(s->index, data, len);
			}
		}
		break;
	}
	default:
		break;
	}
}

/* Handle what came back in an acknowledgement. `*gone` is set when the controller says goodbye,
 * `*alternate` when the controller's protocol version asks for an immediate channel switch. */
static void handle_reply(struct slot *s, const struct esb_payload *rx, bool *gone, bool *alternate)
{
	const uint8_t *d = rx->data;

	switch (d[0]) {
	case PKT_APP_DATA:
		for (int i = 1; i + 2 <= rx->length; i += d[i] + 2) {
			if (i + d[i] + 2 > rx->length) {
				break;
			}
			if (d[i + 1] == REC_SET_RESULT && d[i] != 4) {
				break;      /* malformed: stop reading this packet */
			}
			handle_record(s, &d[i]);
		}
		break;
	case PKT_DISCONNECTING:
		LOG_INF("Slot %d: the controller is disconnecting", s->index);
		*gone = true;
		break;
	case PKT_CONTROLLER_PROTO:
		if (d[1] < 2) {
			s->protocol = d[1];
			LOG_INF("Slot %d: protocol version %u", s->index, d[1]);
			if (s->protocol) {
				*alternate = true;   /* version 1 controllers get their alternates at once */
			}
		}
		break;
	}
}

/* ---- channel switch --------------------------------------------------------------------- */

/* Try `tx` on each channel of the list in turn (0xFF = skip) until one is acknowledged or the
 * time is up. Returns the index that worked, or -1. */
static int try_channels(struct esb_payload *tx, struct esb_payload *rx, const uint32_t *list,
			int count, uint32_t timeout_ms)
{
	k_timepoint_t deadline = sys_timepoint_calc(K_MSEC(timeout_ms));
	int i = 0;

	for (;;) {
		if (list[i] != 0xFF) {
			if (radio_transact(list[i], tx, rx, NULL) >= 0) {
				return i;
			}
			k_usleep(radio_backoff_us());
		}
		if (sys_timepoint_expired(deadline)) {
			return -1;
		}
		i = (i + 1) % count;
	}
}

static void switch_channel(struct slot *s, bool *failed, bool *gone)
{
	struct esb_payload tx, rx;
	uint8_t list[4] = { channels_pick(s->index, s->channel[0]), 0xFF, 0xFF, 0xFF };
	uint32_t try[4] = { s->channel[0], list[0], 0, 0 };
	bool unused;
	int rc;

	if (s->protocol == 0) {
		/* Old controllers get the one new channel, tried on the old and the new for 500 ms. */
		radio_build_channel_change(&tx, s->pipe, list, 1);
		rc = try_channels(&tx, &rx, try, 2, 500);
	} else {
		/* Newer ones also get channels 2 and 80 as standing alternates. */
		list[1] = 2;
		list[2] = 80;
		try[2] = 2;
		try[3] = 80;
		radio_build_channel_change(&tx, s->pipe, list, 3);
		rc = try_channels(&tx, &rx, try, 2, 10);
		if (rc < 0) {
			rc = try_channels(&tx, &rx, try, 4, 500);
		}
	}
	*failed = rc < 0;
	if (rx.length) {
		handle_reply(s, &rx, gone, &unused);
	}
	if (!*failed && !*gone) {
		memcpy(s->channel, list, sizeof(list));
	}
}

/* ---- one exchange while connected ----------------------------------------------------------- */

#define PRIMARY_WINDOW_MS 10
#define GUARD_MS          500

/* Returns 0 to carry on, -1 when the link is lost. */
static int exchange(struct slot *s)
{
	struct esb_payload tx, rx;
	bool gone = false, alternate = false;
	int64_t entry = k_uptime_get();
	int rc = -1, answered_on = -1, misses = 0, next = 0;

	if (s->send_version) {
		radio_build_version(&tx, s->pipe, 1);
		s->send_version = false;
	} else if (s->tell_host_asleep) {
		radio_build_simple(&tx, s->pipe, PKT_HOST_ASLEEP);
		s->tell_host_asleep = false;
	} else if (s->tell_turn_off) {
		radio_build_simple(&tx, s->pipe, PKT_TURN_OFF);
		s->tell_turn_off = false;
	} else {
		uint8_t rec[QUEUE_ITEM + 2];

		radio_build_simple(&tx, s->pipe, PKT_KEEPALIVE);
		while (k_msgq_peek(&s->queue, rec) == 0 && radio_append(&tx, rec, rec[0] + 2) == 0) {
			k_msgq_get(&s->queue, rec, K_NO_WAIT);
		}
	}

	/* Retry on the current channel for 10 ms, then also on the alternates, for up to 500 ms. */
	k_timepoint_t guard = sys_timepoint_calc(K_MSEC(GUARD_MS));
	k_timepoint_t primary = sys_timepoint_calc(K_MSEC(PRIMARY_WINDOW_MS));

	while (!sys_timepoint_expired(guard)) {
		if (!sys_timepoint_expired(primary)) {
			rc = radio_transact(s->channel[0], &tx, &rx, NULL);
			if (rc >= 0) {
				answered_on = 0;
				break;
			}
			misses++;
			k_usleep(radio_backoff_us());
		} else {
			next = (next + 1) & 3;
			if (s->channel[next] == 0xFF) {
				continue;
			}
			rc = radio_transact(s->channel[next], &tx, &rx, NULL);
			if (rc >= 0) {
				answered_on = next;
				break;
			}
			if (next == 0) {
				misses++;
			}
			k_usleep(radio_backoff_us());
		}
	}
	alternate = answered_on > 0;
	s->history = (s->history >> 1) | (misses ? BIT(10) : 0);
	s->retries += misses;

	if (answered_on < 0) {
		LOG_INF("Slot %d: connection timeout", s->index);
		send_quality(s, 3);
		channels_set(s->index, s->channel[0], 0, 10);
		return -1;
	}

	int64_t now = k_uptime_get();

	s->ok++;
	s->rssi_sum += rx.rssi;
	if (s->last_ack_ms && now - s->last_ack_ms > s->max_gap_ms) {
		s->max_gap_ms = (uint16_t)MIN(now - s->last_ack_ms, 0xFFFF);
	}
	s->last_ack_ms = now;
	if (rx.length) {
		handle_reply(s, &rx, &gone, &alternate);
	}
	if (gone) {
		send_quality(s, 4);
		return -1;
	}

	if (!alternate && POPCOUNT(s->history) < 3) {
		/* A healthy link: report now and then, and feed the channel statistics. */
		if (s->window_start_ms + s->qos_interval_ms < entry) {
			channels_set(s->index, s->channel[0], s->ok, s->retries);
			send_quality(s, 0);
		}
		return 0;
	}

	/* Degraded (three or more bad exchanges of the last 11), or only an alternate answered, or a
	 * version 1 controller just said hello: move to a better channel. */
	bool failed = false;

	channels_add(s->index, s->channel[0], s->ok, s->retries);
	send_quality(s, alternate ? 2 : 1);
	s->history = 0;
	switch_channel(s, &failed, &gone);
	if (failed) {
		send_quality(s, 5);
		return -1;
	}
	if (gone) {
		send_quality(s, 4);
		return -1;
	}
	return 0;
}

/* ---- the states ---------------------------------------------------------------------------- */

static void do_pairing(struct slot *s)
{
	struct esb_payload tx, rx;
	char serial[16];
	uint32_t ctrl_id, puck_id;

	/* Pairing always replaces the old controller, so forget it first. */
	struct bond none = { 0 };

	bond_set(s->index, &none);
	k_timepoint_t until = sys_timepoint_calc(K_SECONDS(s->pairing_seconds));

	while (!sys_timepoint_expired(until)) {
		/* A cancel, or the PC going to sleep, ends the window; a changed record is ignored. */
		if (wait_events(s, K_MSEC(10)) & (EV_CANCEL | EV_HOST_SUSPENDED)) {
			break;
		}
		/* Beacon on the pairing channel and address. A controller in pairing mode answers in its
		 * acknowledgement with its ID and serial. */
		radio_build_simple(&tx, 0, PKT_HELLO);
		static const uint8_t pairing_base[4] = { 0x91, 0xA2, 0xA7, 0x93 };

		radio_transact(PAIRING_CHANNEL, &tx, &rx, pairing_base);
		if (rx.length != 21 || rx.data[0] != PKT_PAIRING_REQUEST) {
			continue;
		}
		memcpy(&ctrl_id, &rx.data[1], 4);
		memcpy(serial, &rx.data[5], 16);

		/* Offer: our random ID and serial, sent to an address made from the controller's ID, until
		 * it confirms (at most 500 ms). */
		sys_rand_get(&puck_id, sizeof(puck_id));
		radio_build_pairing_offer(&tx, puck_id, identity_serial());
		k_timepoint_t offer_until = sys_timepoint_calc(K_MSEC(500));

		while (!sys_timepoint_expired(offer_until)) {
			radio_transact(PAIRING_CHANNEL, &tx, &rx, (const uint8_t *)&ctrl_id);
			if (rx.length == 21 && rx.data[0] == PKT_PAIRING_CONFIRM) {
				struct bond b = { .puck_id = puck_id };

				memcpy(&b.controller_id, &rx.data[1], 4);
				memcpy(b.controller_serial, &rx.data[5], 16);
				bond_set(s->index, &b);
				LOG_INF("Slot %d: paired", s->index);
				s->state = CONNECTING;
				send_status(s, 3);
				return;
			}
			k_usleep(radio_backoff_us());
		}
	}
	LOG_INF("Slot %d: pairing failed", s->index);
	s->state = IDLE;
}

static enum state after_stop(struct slot *s)
{
	return bond_valid(bond_get(s->index)) ? CONNECTING : IDLE;
}

static void do_connecting(struct slot *s)
{
	const struct bond *b = bond_get(s->index);
	struct esb_payload tx;
	int tries = 250;
	uint8_t attempt = 0;
	k_timeout_t period = K_MSEC(4);

	LOG_INF("Slot %d: connecting to %.16s", s->index, b->controller_serial);
	radio_bump_prefix(s->pipe);
	s->channel[0] = channels_pick(s->index, 0xFF);
	s->channel[1] = s->channel[2] = s->channel[3] = 0xFF;

	for (;;) {
		uint32_t ev = wait_events(s, period);

		if (ev & EV_STOP) {
			s->state = after_stop(s);
			return;
		}
		if (ev & EV_PAIR) {
			s->state = PAIRING;
			return;
		}
		if (ev & EV_HOST_RESUMED) {
			return;                   /* start again */
		}
		if (ev) {
			continue;                 /* cancel, suspend: nothing to do while connecting */
		}
		/* The period elapsed: try to reach the controller. */
		if (attempt == 0) {
			s->channel[0] = channels_pick(s->index, 0xFF);   /* every 256 tries a fresh channel */
		}
		/* 1. A broadcast on channel 2 to the common address says where to find us. */
		radio_build_connect_request(&tx, b->puck_id, b->controller_id, s->channel[0],
					    radio_prefix(s->pipe));
		struct esb_payload rx;

		radio_transact(CONNECT_CHANNEL, &tx, &rx, NULL);
		k_msleep(1);
		/* 2. A hello on our own pipe and channel; an acknowledgement means it heard us. */
		radio_build_simple(&tx, s->pipe, PKT_HELLO);
		if (radio_transact(s->channel[0], &tx, &rx, NULL) >= 0) {
			s->state = CONNECTED;
			return;
		}
		if (--tries == 0) {
			period = K_MSEC(100);     /* after about a second of fast tries, slow down */
		}
		attempt++;
	}
}

static void queue_clear(struct slot *s)
{
	k_msgq_purge(&s->queue);
}

static void fr_reset(struct slot *s)
{
	k_timer_stop(&s->fr_timer);
	s->fr_state = 0;
}

static void do_connected(struct slot *s)
{
	uint32_t ev;

	LOG_INF("Slot %d: connected to %.16s on channel %u", s->index,
		bond_get(s->index)->controller_serial, s->channel[0]);
	queue_clear(s);
	s->send_version = true;
	s->protocol = 0;
	fr_reset(s);
	combiner_reset(s->index);
	s->connected_at_ms = k_uptime_get();
	s->history = 0;
	s->last_ack_ms = 0;
	window_reset(s, s->connected_at_ms);

	if (s->host_suspended) {
		LOG_INF("Slot %d: the PC is asleep", s->index);
		if (!s->wake_blocked) {
			usb_remote_wakeup();
			send_status(s, 2);
			k_timer_start(&s->timer, K_MSEC(10000), K_NO_WAIT);   /* wait for the PC to wake */
		}
		for (;;) {
			ev = wait_events(s, K_MSEC(3));
			if (ev & EV_STOP) {
				s->state = after_stop(s);
				break;
			}
			if (ev & EV_HOST_RESUMED) {
				break;
			}
			if (ev & EV_TIMER) {
				LOG_WRN("Slot %d: the PC did not wake up", s->index);
				s->tell_host_asleep = true;
				k_timer_stop(&s->timer);
			}
			if (exchange(s) != 0) {
				s->state = CONNECTING;
				break;
			}
		}
		k_timer_stop(&s->timer);
	}
	s->wake_blocked = false;
	if (s->state != CONNECTED) {
		goto leave;
	}

	send_status(s, 2);
	LOG_INF("Slot %d: the PC is awake", s->index);
	for (;;) {
		ev = wait_events(s, K_MSEC(3));
		if (ev & EV_PAIR) {
			s->state = PAIRING;
			break;
		}
		if (ev & EV_STOP) {
			s->state = after_stop(s);
			break;
		}
		if (ev & EV_HOST_SUSPENDED) {
			k_timer_start(&s->timer, K_MSEC(2000), K_NO_WAIT);
		}
		if (ev & EV_HOST_RESUMED) {
			k_timer_stop(&s->timer);
		}
		if (ev & EV_TIMER) {
			/* The PC has been asleep for 2 s: tell the controller to switch off. */
			LOG_INF("Slot %d: the PC is asleep, turning the controller off", s->index);
			s->tell_turn_off = true;
			k_timer_start(&s->timer, K_MSEC(2000), K_NO_WAIT);
			for (;;) {
				ev = wait_events(s, K_MSEC(3));
				if (ev & EV_STOP) {
					s->state = after_stop(s);
					break;
				}
				if (exchange(s) != 0) {
					s->state = CONNECTING;
					break;
				}
				if (ev & EV_TIMER) {
					LOG_INF("Slot %d: the controller did not switch off, disconnecting",
						s->index);
					s->wake_blocked = true;
					s->state = CONNECTING;
					break;
				}
			}
			break;
		}
		combiner_tick(s->index);
		if (exchange(s) != 0) {
			s->state = CONNECTING;
			break;
		}
	}
	k_timer_stop(&s->timer);
leave:
	release_keys_and_buttons(s);
	send_status(s, 1);
	combiner_reset(s->index);
	s->send_version = s->tell_host_asleep = s->tell_turn_off = false;
}

static void slot_thread(void *p1, void *p2, void *p3)
{
	struct slot *s = p1;

	s->state = IDLE;
	k_event_post(&s->events, EV_STOP);   /* start by looking at the pairing record */
	for (;;) {
		switch (s->state) {
		case IDLE:
			if (s->last_logged != IDLE) {
				LOG_INF("Slot %d: idle", s->index);
			}
			s->last_logged = IDLE;
			for (;;) {
				uint32_t ev = wait_events(s, K_FOREVER);

				if (ev & EV_STOP) {
					s->state = after_stop(s);
					break;
				}
				if (ev & EV_PAIR) {
					s->state = PAIRING;
					break;
				}
			}
			break;
		case PAIRING:
			s->last_logged = PAIRING;
			LOG_INF("Slot %d: pairing", s->index);
			do_pairing(s);
			break;
		case CONNECTING:
			do_connecting(s);
			break;
		case CONNECTED:
			do_connected(s);
			break;
		}
		k_timer_stop(&s->timer);
		k_event_clear(&s->events, EV_TIMER);
	}
}

static void on_timer(struct k_timer *t)
{
	k_event_post(&CONTAINER_OF(t, struct slot, timer)->events, EV_TIMER);
}

static void on_fr_timer(struct k_timer *t)
{
	/* The controller did not answer a feature report request in time; the next GET starts over. */
}

void slots_init(void)
{
	for (int i = 0; i < NUM_SLOTS; i++) {
		struct slot *s = &slots[i];

		s->index = i;
		s->pipe = i + 1;
		s->host_suspended = true;
		s->qos_interval_ms = 500;
		k_event_init(&s->events);
		k_timer_init(&s->timer, on_timer, NULL);
		k_timer_init(&s->fr_timer, on_fr_timer, NULL);
		k_mutex_init(&s->fr_lock);
		k_msgq_init(&s->queue, (char *)s->queue_buf, QUEUE_ITEM, QUEUE_DEPTH);
	}
}

void slots_start(void)
{
	for (int i = 0; i < NUM_SLOTS; i++) {
		k_thread_create(&slots[i].thread, slot_stacks[i], K_THREAD_STACK_SIZEOF(slot_stacks[i]),
				slot_thread, &slots[i], NULL, NULL, 5, 0, K_NO_WAIT);
		k_thread_name_set(&slots[i].thread, "slot");
	}
}

/* ---- from the USB side and the commands ------------------------------------------------- */

void slot_host_state(int slot, bool suspended)
{
	struct slot *s = &slots[slot];

	s->host_suspended = suspended;
	k_event_post(&s->events, suspended ? EV_HOST_SUSPENDED : EV_HOST_RESUMED);
}

int slot_feature_set(int slot, const uint8_t *body)
{
	struct slot *s = &slots[slot];
	uint8_t rec[QUEUE_ITEM] = { 0 };
	int rc = -EAGAIN;
	uint8_t len = MIN((unsigned int)body[1] + 2, FEATURE_BODY_LEN);

	k_mutex_lock(&s->fr_lock, K_FOREVER);
	if (s->state == CONNECTED) {
		fr_reset(s);
		rec[0] = len;
		rec[1] = REC_SET_FEATURE;
		memcpy(&rec[2], body, len);
		if (k_msgq_put(&s->queue, rec, K_NO_WAIT) == 0) {
			s->fr_opcode = body[0];
			s->fr_state = 1;
			rc = 0;
		}
	} else {
		fr_reset(s);
	}
	k_mutex_unlock(&s->fr_lock);
	return rc;
}

/* Reading is two steps: the first read sends the request to the controller and says "try again";
 * a later read returns the answer once it has arrived. */
int slot_feature_get(int slot, uint8_t *body)
{
	struct slot *s = &slots[slot];
	int rc = -EAGAIN;

	k_mutex_lock(&s->fr_lock, K_FOREVER);
	if (s->state != CONNECTED) {
		fr_reset(s);
	} else if (s->fr_state == 1) {
		uint8_t rec[QUEUE_ITEM] = { 1, REC_GET_FEATURE, s->fr_opcode };

		if (k_msgq_put(&s->queue, rec, K_NO_WAIT) == 0) {
			k_timer_start(&s->fr_timer, K_MSEC(1000), K_NO_WAIT);
			s->fr_state = 2;
		}
	} else if (s->fr_state == 2) {
		if (k_timer_status_get(&s->fr_timer)) {
			LOG_WRN("Slot %d: the controller did not answer a feature report", s->index);
			fr_reset(s);
		}
	} else if (s->fr_state == 3) {
		if (s->fr_answer[0] == s->fr_opcode) {
			memcpy(body, s->fr_answer, FEATURE_BODY_LEN);
			rc = 0;
		}
		fr_reset(s);
	} else {
		fr_reset(s);
	}
	k_mutex_unlock(&s->fr_lock);
	return rc;
}

int slot_output_report(int slot, const uint8_t *data, size_t len)
{
	struct slot *s = &slots[slot];
	uint8_t rec[QUEUE_ITEM] = { 0 };

	if (s->state != CONNECTED) {
		return -EAGAIN;
	}
	if (len > 64) {
		return -EINVAL;
	}
	rec[0] = len;
	rec[1] = REC_OUTPUT_REPORT;
	memcpy(&rec[2], data, len);
	(void)k_msgq_put(&s->queue, rec, K_NO_WAIT);   /* a full queue drops the report */
	return 0;
}

bool slot_connected(int slot) { return slots[slot].state == CONNECTED; }

void slot_start_pairing(int slot, uint8_t seconds)
{
	slots[slot].pairing_seconds = seconds;
	k_event_post(&slots[slot].events, EV_PAIR);
}

void slot_cancel_pairing(int slot)
{
	k_event_post(&slots[slot].events, EV_CANCEL);
}

void slot_set_bond(int slot, const struct bond *b)
{
	bond_set(slot, b);
	k_event_post(&slots[slot].events, EV_STOP);
}

const char *slot_state_name(int slot) { return state_names[slots[slot].state]; }

int slot_info(int slot, uint8_t *channels, uint32_t *uptime_s)
{
	struct slot *s = &slots[slot];

	memcpy(channels, s->channel, 4);
	*uptime_s = s->state == CONNECTED ? (uint32_t)((k_uptime_get() - s->connected_at_ms) / 1000) : 0;
	return s->state;
}

void slot_set_qos(int slot, uint16_t interval_ms, bool print)
{
	slots[slot].qos_interval_ms = interval_ms;
	slots[slot].qos_print = print;
}
