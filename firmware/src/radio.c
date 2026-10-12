/* radio.c: the link to the controllers, on Nordic's Enhanced ShockBurst. MIT licence. */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>
#include <zephyr/random/random.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/sys/onoff.h>
#include <cmsis_core.h>
#include <hal/nrf_radio.h>
#include <hal/nrf_clock.h>
#include "radio.h"
#include "puck_config.h"

LOG_MODULE_REGISTER(radio, LOG_LEVEL_INF);

/* Addresses. An ESB address is a 4-byte base plus a one-byte prefix per pipe. Pipe 0 uses base 0,
 * pipes 1 to 7 use base 1.
 *  - base 0 is the text "ibex", the same on every puck: the common meeting address;
 *  - base 1 is random, chosen at every start: this puck's own address;
 *  - slot n talks on pipe n + 1; its prefix grows by 0x10 every time it starts connecting, so each
 *    connection uses a fresh address. */
static const uint8_t common_base[4] = { 'i', 'b', 'e', 'x' };
static uint8_t own_base[4];
static uint8_t prefixes[5] = { 0x10, 0x01, 0x02, 0x03, 0x04 };

static K_MUTEX_DEFINE(radio_lock);
static K_SEM_DEFINE(esb_event, 0, 1);
static volatile uint32_t esb_seen;   /* bit mask of ESB event IDs seen in this transaction */

static void on_esb_event(const struct esb_evt *evt)
{
	esb_seen |= BIT(evt->evt_id);
	k_sem_give(&esb_event);
}

static enum esb_tx_power tx_power(void)
{
	switch (PUCK_TX_POWER_DBM) {
	case 8: return ESB_TX_POWER_8DBM;
	case 7: return ESB_TX_POWER_7DBM;
	case 6: return ESB_TX_POWER_6DBM;
	case 5: return ESB_TX_POWER_5DBM;
	case 4: return ESB_TX_POWER_4DBM;
	case 3: return ESB_TX_POWER_3DBM;
	case 2: return ESB_TX_POWER_2DBM;
	default: return ESB_TX_POWER_0DBM;
	}
}

int radio_start(void)
{
	struct esb_config cfg = ESB_DEFAULT_CONFIG;
	struct onoff_manager *clk = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
	struct onoff_client clk_client;
	int err, clk_result;

	/* The radio needs the 32 MHz crystal running: ask for it and wait until it is. */
	sys_notify_init_spinwait(&clk_client.notify);
	err = onoff_request(clk, &clk_client);
	if (err < 0) {
		LOG_ERR("Requesting the 32 MHz clock failed (%d)", err);
		return err;
	}
	/* Wait without hogging the CPU, and not forever. */
	for (int ms = 0; sys_notify_fetch_result(&clk_client.notify, &clk_result); ms++) {
		if (ms >= 500) {
			bool running = nrf_clock_hf_is_running(NRF_CLOCK, NRF_CLOCK_HFCLK_HIGH_ACCURACY);

			LOG_WRN("The 32 MHz clock request did not complete (crystal %s)",
				running ? "is running, carrying on" : "is NOT running");
			if (!running) {
				return -ETIMEDOUT;
			}
			break;
		}
		k_msleep(1);
	}

	/* Four random bytes, avoiding values that look like the radio's preamble or idle pattern. */
	sys_rand_get(own_base, sizeof(own_base));
	for (int i = 0; i < 4; i++) {
		while (own_base[i] == 0x00 || own_base[i] == 0xAA || own_base[i] == 0x55) {
			sys_rand_get(&own_base[i], 1);
		}
	}

	cfg.protocol = ESB_PROTOCOL_ESB_DPL;           /* dynamic payload length */
	cfg.mode = ESB_MODE_PTX;                       /* the puck sends, the controllers answer */
	cfg.event_handler = on_esb_event;
	cfg.bitrate = ESB_BITRATE_2MBPS_BLE;
	cfg.crc = ESB_CRC_16BIT;
	cfg.tx_output_power = tx_power();
	cfg.retransmit_delay = 600;
	cfg.retransmit_count = 0;                      /* one attempt per packet; slots retry in software */
	cfg.tx_mode = ESB_TXMODE_AUTO;
	cfg.payload_length = 128;
	cfg.selective_auto_ack = true;
	cfg.use_fast_ramp_up = true;
	err = esb_init(&cfg);
	if (err) {
		LOG_ERR("esb_init failed (%d)", err);
		return err;
	}
	esb_set_base_address_0(common_base);
	esb_set_base_address_1(own_base);
	esb_set_prefixes(prefixes, ARRAY_SIZE(prefixes));
	return 0;
}

/* Flash writes stall the CPU, long enough to make a radio transaction time out: hold the radio
 * (no transaction can start) around them. */
void radio_hold(void) { k_mutex_lock(&radio_lock, K_FOREVER); }
void radio_release(void) { k_mutex_unlock(&radio_lock); }

const uint8_t *radio_own_address(void) { return own_base; }
uint8_t radio_prefix(int pipe) { return prefixes[pipe]; }

void radio_bump_prefix(int pipe)
{
	/* Never while a transaction is on the air: the radio would refuse the change. */
	k_mutex_lock(&radio_lock, K_FOREVER);
	prefixes[pipe] += 0x10;
	esb_set_prefixes(prefixes, ARRAY_SIZE(prefixes));
	k_mutex_unlock(&radio_lock);
}

/* Wait for ESB to be idle, for at most 5 ms; if it is not by then the radio is stuck, so reset. */
static void wait_idle_or_reset(void)
{
	int64_t start = k_uptime_get();

	while (!esb_is_idle()) {
		if (k_uptime_get() - start >= 5) {
			LOG_ERR("The radio is stuck, resetting");
			NVIC_SystemReset();
		}
		k_busy_wait(50);
	}
}

int radio_transact(uint32_t channel, struct esb_payload *tx, struct esb_payload *rx,
		   const uint8_t *base)
{
	int err, result = 0;

	rx->length = 0;
	k_mutex_lock(&radio_lock, K_FOREVER);
	k_sem_take(&esb_event, K_NO_WAIT);   /* drop a stale wake-up */
	esb_flush_rx();                      /* and any acknowledgement payload nobody read */

	err = esb_set_rf_channel(channel);
	if (base) {
		err |= (tx->pipe == 0) ? esb_set_base_address_0(base) : esb_set_base_address_1(base);
	}
	esb_seen = 0;
	err |= esb_write_payload(tx);        /* goes out at once */

	/* Wait up to 20 ms for the radio to report (not at all if it could not even be set up). */
	if (err || k_sem_take(&esb_event, K_MSEC(20))) {
		LOG_ERR("No event from ESB (setup %d): type 0x%02X pipe %u noack %u channel %u, seen 0x%X",
			err, tx->data[0], tx->pipe, tx->noack, channel, esb_seen);
		nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
		wait_idle_or_reset();
		esb_flush_tx();   /* a packet left in the queue would make every later write fail */
		esb_flush_rx();
	}
	wait_idle_or_reset();   /* the acknowledgement payload event follows the success event */

	if (esb_seen & BIT(ESB_EVENT_RX_RECEIVED)) {
		esb_read_rx_payload(rx);
		result = 1;
	} else {
		rx->rssi = nrf_radio_rssi_sample_get(NRF_RADIO) & 0x7F;
		if (!(esb_seen & BIT(ESB_EVENT_TX_SUCCESS))) {
			result = -1;
		}
	}
	if (esb_seen & BIT(ESB_EVENT_TX_FAILED)) {
		esb_flush_tx();
		if (tx->pipe) {
			esb_reuse_pid(tx->pipe);
		}
		result = -1;
	}
	if (base) {                           /* back to the usual addresses */
		esb_set_base_address_0(common_base);
		esb_set_base_address_1(own_base);
	}
	k_mutex_unlock(&radio_lock);
	return result;
}

void radio_sample_rssi(uint8_t channel, uint8_t *out, int n)
{
	/* With the radio idle and its interrupt held off, tune to the channel, start receiving, and
	 * read the signal strength n times, then put everything back as ESB expects it. */
	k_mutex_lock(&radio_lock, K_FOREVER);
	irq_disable(RADIO_IRQn);
	uint32_t shorts = nrf_radio_shorts_get(NRF_RADIO);
	uint32_t addresses = nrf_radio_rxaddresses_get(NRF_RADIO);
	bool ok = true;

	memset(out, 0x7F, n);                    /* "nothing heard" if the scan has to give up */
	nrf_radio_rxaddresses_set(NRF_RADIO, 0); /* no packet may match while scanning */
	nrf_radio_shorts_set(NRF_RADIO, NRF_RADIO_SHORT_READY_START_MASK |
					NRF_RADIO_SHORT_DISABLED_RSSISTOP_MASK);
	nrf_radio_frequency_set(NRF_RADIO, 2400 + channel);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
	for (int spin = 0; nrf_radio_state_get(NRF_RADIO) != NRF_RADIO_STATE_RX; spin++) {
		if (spin > 2000) {
			ok = false;
			break;
		}
		k_busy_wait(1);
	}
	for (int i = 0; ok && i < n; i++) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_RSSIEND);
		nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTART);
		for (int spin = 0; !nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_RSSIEND); spin++) {
			if (spin > 2000) {
				ok = false;
				break;
			}
			k_busy_wait(1);
		}
		if (ok) {
			out[i] = nrf_radio_rssi_sample_get(NRF_RADIO) & 0x7F;
		}
	}
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
	for (int spin = 0; nrf_radio_state_get(NRF_RADIO) != NRF_RADIO_STATE_DISABLED; spin++) {
		if (spin > 2000) {
			LOG_ERR("The radio did not stop after a noise scan");
			break;
		}
		k_busy_wait(1);
	}
	nrf_radio_rxaddresses_set(NRF_RADIO, addresses);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_RSSIEND);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_READY);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	nrf_radio_shorts_set(NRF_RADIO, shorts);
	NVIC_ClearPendingIRQ(RADIO_IRQn);
	irq_enable(RADIO_IRQn);
	k_mutex_unlock(&radio_lock);
}

int32_t radio_backoff_us(void)
{
	static const int32_t pause[4] = { 1500, 500, 1000, 1000 };

	return pause[sys_rand32_get() & 3];
}

/* ---- packet builders ---------------------------------------------------------------------- */

static void start(struct esb_payload *p, uint8_t pipe, uint8_t type, uint8_t len)
{
	memset(p, 0, sizeof(*p));
	p->pipe = pipe;
	p->data[0] = type;
	p->length = len;
}

void radio_build_simple(struct esb_payload *p, uint8_t pipe, uint8_t type)
{
	start(p, pipe, type, 1);
}

void radio_build_version(struct esb_payload *p, uint8_t pipe, uint8_t version)
{
	start(p, pipe, PKT_PROTOCOL_VERSION, 3);
	p->data[1] = 0;
	p->data[2] = version;
}

void radio_build_connect_request(struct esb_payload *p, uint32_t puck_id, uint32_t ctrl_id,
				 uint32_t channel, uint8_t prefix)
{
	start(p, 0, PKT_CONNECT_REQUEST, 18);
	p->noack = 1;                        /* a broadcast: nobody acknowledges it */
	memcpy(&p->data[1], &puck_id, 4);
	memcpy(&p->data[5], &ctrl_id, 4);
	memcpy(&p->data[9], &channel, 4);
	memcpy(&p->data[13], own_base, 4);
	p->data[17] = prefix;
}

void radio_build_pairing_offer(struct esb_payload *p, uint32_t puck_id, const char *serial)
{
	start(p, 0, PKT_PAIRING_OFFER, 21);
	memcpy(&p->data[1], &puck_id, 4);
	strncpy((char *)&p->data[5], serial, 16);
}

void radio_build_channel_change(struct esb_payload *p, uint8_t pipe, const uint8_t *channels,
				uint8_t count)
{
	start(p, pipe, PKT_CHANNEL_CHANGE, count + 1);
	memcpy(&p->data[1], channels, count);
}

int radio_append(struct esb_payload *p, const uint8_t *rec, int rec_len)
{
	if (128 - p->length < rec_len) {
		return -1;
	}
	memcpy(&p->data[p->length], rec, rec_len);
	p->length += rec_len;
	return 0;
}
