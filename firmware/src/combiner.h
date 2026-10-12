/* combiner.h: merges the controller's two half-reports (0x60, 0x61) into report 0x45.
 * The formats are in docs/wire-formats.md. MIT licence. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* A half-report arrived on a slot (rec[0] is 0x60 or 0x61, 20 bytes): update and send 0x45. */
void combiner_half(int slot, const uint8_t *rec);
/* Call regularly: after a second without a half-report a neutral 0x45 is sent. */
void combiner_tick(int slot);
/* Forget everything about a slot (it disconnected or reconnected). */
void combiner_reset(int slot);
