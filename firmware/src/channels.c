/* channels.c: channel choice from per-channel error rates and a background noise scan. MIT licence. */
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include "channels.h"
#include "radio.h"
#include "usb.h"

#define FIRST_CHANNEL 4
#define LAST_CHANNEL  80
#define N_CHANNELS    (LAST_CHANNEL + 1)

#define WEIGHT_ERROR_RATE 1000
#define WEIGHT_DISTANCE   1000
#define WEIGHT_NOISE      500

#define SCAN_PERIOD_MS    128
#define SCAN_SAMPLES      10

struct chan {
	int ok, bad;
	int rate_permille;   /* bad / (ok + bad), 0..1000 */
};

static struct chan map[NUM_SLOTS][N_CHANNELS];

/* Background noise per channel, in 16.16 fixed point, as positive dB below 0 dBm (bigger = quieter):
 * a slow average, and a peak that jumps to a louder reading at once and drifts back slowly. */
static struct { uint32_t average, peak; } noise[N_CHANNELS];
static uint8_t quietest;     /* the quietest average seen over all channels */
static bool noise_known;
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

/* Penalty for a channel that is `d` dB noisier than the quietest. A curve that starts gently and
 * steepens to a straight line, measured against the reference behaviour at every tenth dB and
 * joined by straight lines: 0 at 0 dB, 31 at 10, 89 at 20, 164 at 30, 252 at 40, 353 at 50,
 * 464 at 60, 585 at 70, 715 at 80, 853 at 90, 1000 at 100 and 1137 at 109. */
static int noise_penalty(int d)
{
	static const int at_tens[] = { 0, 31, 89, 164, 252, 353, 464, 585, 715, 853, 1000 };

	d = CLAMP(d, 0, 109);
	if (d >= 100) {
		return 1000 + (d - 100) * 137 / 9;
	}
	return at_tens[d / 10] + (at_tens[d / 10 + 1] - at_tens[d / 10]) * (d % 10) / 10;
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
		score += WEIGHT_NOISE * noise_penalty(MAX((int)quietest - (int)(noise[ch].average >> 16), 0));
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

uint8_t channels_noise(uint8_t channel) { return channel < N_CHANNELS ? (noise[channel].average >> 16) & 0xFF : 0; }
uint8_t channels_noise_peak(uint8_t channel) { return channel < N_CHANNELS ? (noise[channel].peak >> 16) & 0xFF : 0; }

/* A bad experience on a channel is slowly forgiven: every minute all error rates shrink by 10 %. */
static void decay(struct k_timer *t)
{
	for (int s = 0; s < NUM_SLOTS; s++) {
		for (int ch = FIRST_CHANNEL; ch <= LAST_CHANNEL; ch += 2) {
			/* the stored rate shrinks; the counters are left alone, and the next update of this
			 * channel works the rate out from them again */
			map[s][ch].rate_permille = map[s][ch].rate_permille * 9 / 10;
		}
	}
}
static K_TIMER_DEFINE(decay_timer, decay, NULL);

/* One sweep: the strongest of ten readings on every channel, folded into the averages. */
static void scan_once(void)
{
	uint8_t samples[SCAN_SAMPLES];
	uint32_t best = 0;

	for (int ch = FIRST_CHANNEL; ch <= LAST_CHANNEL; ch += 2) {
		uint32_t strongest = 0xFF;

		radio_sample_rssi(ch, samples, SCAN_SAMPLES);
		for (int i = 0; i < SCAN_SAMPLES; i++) {
			strongest = MIN(strongest, samples[i]);   /* smaller = louder */
		}
		uint32_t v = strongest << 16;

		if (!noise_known) {
			noise[ch].average = noise[ch].peak = v;
		} else {
			noise[ch].average = (noise[ch].average * 63 + v) >> 6;
			noise[ch].peak = (noise[ch].peak <= v) ? (noise[ch].peak * 63 + v) >> 6 : v;
		}
		best = MAX(best, noise[ch].average);
	}
	quietest = best >> 16;
	noise_known = true;
}

static void scan_thread(void *a, void *b, void *c)
{
	for (;;) {
		scan_once();
		k_msleep(SCAN_PERIOD_MS);
	}
}
K_THREAD_DEFINE(noise_scan, 1536, scan_thread, NULL, NULL, NULL, 10, 0, -1);

void channels_init(void)
{
	memset(map, 0, sizeof(map));
	k_timer_start(&decay_timer, K_SECONDS(60), K_SECONDS(60));
}

/* Called once the radio runs: one sweep first so the first channel choice already knows the noise,
 * then the scan keeps going. */
void channels_start_scan(void)
{
	scan_once();
	k_thread_start(noise_scan);
}
