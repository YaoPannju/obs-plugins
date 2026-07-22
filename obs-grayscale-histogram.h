#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#define HISTOGRAM_BINS 256

#ifdef __cplusplus
extern "C" {
#endif

struct histogram_data {
	uint32_t bins[HISTOGRAM_BINS];
	uint32_t total_pixels;
	uint64_t last_update_ns;
	bool valid;
};

struct histogram_shared_state {
	struct histogram_data data;
	pthread_mutex_t mutex;
};

extern struct histogram_shared_state g_histogram_state;

#ifdef __cplusplus
}
#endif