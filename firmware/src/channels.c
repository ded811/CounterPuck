/* channels.c: channel choice from per-channel error rates. MIT licence.
 * Not done yet: the background noise scan (the noise statistics read 0). */
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include "channels.h"
#include "usb.h"

#define FIRST_CHANNEL 4
#define LAST_CHANNEL  80
#define N_CHANNELS    (LAST_CHANNEL + 1)

#define WEIGHT_ERROR_RATE 1000
#define WEIGHT_DISTANCE   1000

struct chan {
	int ok, bad;
	int rate_permille;   /* bad / (ok + bad), 0..1000 */
};

static struct chan map[NUM_SLOTS][N_CHANNELS];
static K_MUTEX_DEFINE(map_lock);

static void recompute(struct chan *c)
{
	c->rate_permille = (c->ok + c->bad) ? (int)((int64_t)c->bad * 1000 / (c->ok + c->bad)) : 1000;
}

void channels_set(int slot, uint8_t channel, int ok, int bad)
{
	if (channel >= N_CHANNELS) {
		return;
	}
	k_mutex_lock(&map_lock, K_FOREVER);
	map[slot][channel].ok = ok;
	map[slot][channel].bad = bad;
	recompute(&map[slot][channel]);
	k_mutex_unlock(&map_lock);
}

void channels_add(int slot, uint8_t channel, int ok, int bad)
{
	if (channel >= N_CHANNELS) {
		return;
	}
	k_mutex_lock(&map_lock, K_FOREVER);
	map[slot][channel].ok += ok;
	map[slot][channel].bad += bad;
	recompute(&map[slot][channel]);
	k_mutex_unlock(&map_lock);
}

uint8_t channels_pick(int slot, uint8_t current)
{
	uint8_t best = FIRST_CHANNEL;
	uint32_t best_score = UINT32_MAX;

	k_mutex_lock(&map_lock, K_FOREVER);
	for (int ch = FIRST_CHANNEL; ch <= LAST_CHANNEL; ch += 2) {
		uint32_t score;
		int distance = abs(ch - current);

		if (ch == LAST_CHANNEL || ch == current) {
			continue;
		}
		score = WEIGHT_ERROR_RATE * MIN(map[slot][ch].rate_permille / 10, 100);
		if (distance <= 16) {
			score += WEIGHT_DISTANCE * (17 - distance);
		}
		if (score <= best_score) {   /* ties go to the higher channel */
			best_score = score;
			best = ch;
		}
	}
	k_mutex_unlock(&map_lock);
	return best;
}

uint8_t channels_error_percent(int slot, uint8_t channel)
{
	if (channel >= N_CHANNELS) {
		return 0xFF;
	}
	return MIN(map[slot][channel].rate_permille / 10, 100);
}

uint8_t channels_noise(uint8_t channel) { return 0; }
uint8_t channels_noise_peak(uint8_t channel) { return 0; }

/* A bad experience on a channel is slowly forgiven: every minute all error rates shrink by 10 %. */
static void decay(struct k_timer *t)
{
	for (int s = 0; s < NUM_SLOTS; s++) {
		for (int ch = FIRST_CHANNEL; ch <= LAST_CHANNEL; ch += 2) {
			map[s][ch].rate_permille = map[s][ch].rate_permille * 9 / 10;
		}
	}
}
static K_TIMER_DEFINE(decay_timer, decay, NULL);

void channels_init(void)
{
	memset(map, 0, sizeof(map));
	k_timer_start(&decay_timer, K_SECONDS(60), K_SECONDS(60));
}
