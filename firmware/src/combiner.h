/* combiner.h: merges the controller's two half-reports (0x60, 0x61) into report 0x45.
 * The formats are in docs/wire-formats.md. MIT licence. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* There is one combined state for the whole puck. Reports go to the lowest-numbered connected slot
 * whose controller sends half-reports. */

/* A half-report arrived on a slot (rec[0] is 0x60 or 0x61, 20 bytes): update and send 0x45. */
void combiner_half(int slot, const uint8_t *rec);
/* Call regularly: after a second without a half-report a neutral 0x45 is sent. */
void combiner_tick(int slot);
/* The slot's controller is gone (link lost, or a new connection starts). If it was being served, a
 * neutral 0x45 goes out first so the PC is not left holding its last buttons and sticks. */
void combiner_release(int slot);
