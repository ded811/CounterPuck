/* radio.h: the 2.4 GHz link to the controllers, on Nordic's Enhanced ShockBurst. MIT licence. */
#pragma once
#include <stdint.h>
#include <esb.h>

/* Packet types, the first byte of every payload. See docs/wire-formats.md. */
#define PKT_PAIRING_OFFER    0xE0
#define PKT_CONNECT_REQUEST  0xE1
#define PKT_HELLO            0xE2
#define PKT_KEEPALIVE        0xE3
#define PKT_CHANNEL_CHANGE   0xE4
#define PKT_TURN_OFF         0xE5
#define PKT_HOST_ASLEEP      0xE6
#define PKT_PROTOCOL_VERSION 0xE7
#define PKT_PAIRING_REQUEST  0xF0
#define PKT_APP_DATA         0xF1
#define PKT_DISCONNECTING    0xF2
#define PKT_CONTROLLER_PROTO 0xF3
#define PKT_PAIRING_CONFIRM  0xF4

#define PAIRING_CHANNEL 2
#define CONNECT_CHANNEL 2

int radio_start(void);

/* One transaction: send `tx` on `channel` and wait for the acknowledgement. Returns 1 when the
 * acknowledgement carried a payload (copied to `rx`), 0 when it did not, -1 when nobody answered.
 * `rx->rssi` is the signal strength as a positive number of dB below 0 dBm.
 * `base` (4 bytes), when given, replaces the base address of the packet's pipe for this one
 * transaction. The radio is shared: transactions of all slots are serialised. */
int radio_transact(uint32_t channel, struct esb_payload *tx, struct esb_payload *rx,
		   const uint8_t *base);

/* Listen on `channel` for a moment and take `n` signal strength samples (each a positive number of dB
 * below 0 dBm: bigger means quieter). Waits for any transaction in progress. */
void radio_sample_rssi(uint8_t channel, uint8_t *out, int n);

/* This puck's own base address (random, chosen at every start) and the pipe prefixes. */
const uint8_t *radio_own_address(void);
uint8_t radio_prefix(int pipe);
void radio_bump_prefix(int pipe);

/* A short random pause to sleep after a lost packet, in microseconds. */
int32_t radio_backoff_us(void);

/* Packet builders: `p` is cleared first. */
void radio_build_simple(struct esb_payload *p, uint8_t pipe, uint8_t type);
void radio_build_version(struct esb_payload *p, uint8_t pipe, uint8_t version);
void radio_build_connect_request(struct esb_payload *p, uint32_t puck_id, uint32_t ctrl_id,
				 uint32_t channel, uint8_t prefix);
void radio_build_pairing_offer(struct esb_payload *p, uint32_t puck_id, const char *serial);
void radio_build_channel_change(struct esb_payload *p, uint8_t pipe, const uint8_t *channels,
				uint8_t count);
/* Append a record {len, type, data} (rec points at len); returns 0, or -1 when it does not fit. */
int radio_append(struct esb_payload *p, const uint8_t *rec, int rec_len);
