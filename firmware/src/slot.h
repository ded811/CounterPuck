/* slot.h: one radio slot per controller. Each has a thread that pairs, connects and keeps the
 * link going. See docs/puck-internals.md for the behaviour. MIT licence. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bonds.h"

void slots_init(void);     /* make the slots; must run before the USB device is enabled */
void slots_start(void);    /* start their threads; the radio must be running */

/* From the USB side. */
void slot_host_state(int slot, bool suspended);
int slot_feature_set(int slot, const uint8_t *body);      /* feature report 1 from the PC */
int slot_feature_get(int slot, uint8_t *body);            /* feature report 1 to the PC */
int slot_output_report(int slot, const uint8_t *data, size_t len);

/* From the vendor commands and the shell. */
bool slot_connected(int slot);
void slot_start_pairing(int slot, uint8_t seconds);
void slot_cancel_pairing(int slot);
void slot_set_bond(int slot, const struct bond *b);       /* store or delete, then restart the slot */
const char *slot_state_name(int slot);
int slot_info(int slot, uint8_t *channels /* 4 */, uint32_t *uptime_s);
void slot_trace_output(bool on);   /* log every output report (rumble, LEDs) the PC sends */
void slot_set_qos(int slot, uint16_t interval_ms, bool print);
