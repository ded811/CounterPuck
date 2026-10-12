/* channels.h: how the puck chooses radio channels. MIT licence.
 *
 * Data channels are the even channels 4 to 78 (channel n is 2400 + n MHz). Each slot keeps, per
 * channel, how many of its exchanges succeeded and failed. The lowest score wins:
 *   - the packet error rate on the channel, weighted 1000 per percent;
 *   - a penalty for channels close to the current one (so a switch moves away from interference),
 *     1000 x (17 - distance) for distances up to 16, none beyond;
 *   - how much noisier the channel is than the quietest one, from a background scan that visits
 *     every channel eight times a second, weighted 500;
 * the current channel and channel 80 are never chosen. Ties go to the higher channel. */
#pragma once
#include <stdint.h>

void channels_init(void);
void channels_start_scan(void);   /* after radio_start() */
/* Replace / add to a channel's counters of one slot. */
void channels_set(int slot, uint8_t channel, int ok, int bad);
void channels_add(int slot, uint8_t channel, int ok, int bad);
/* `current` is the channel in use, or 0xFF when there is none. */
uint8_t channels_pick(int slot, uint8_t current);
/* Statistics for the vendor command that lists them. */
uint8_t channels_error_percent(int slot, uint8_t channel);
uint8_t channels_noise(uint8_t channel);
uint8_t channels_noise_peak(uint8_t channel);
