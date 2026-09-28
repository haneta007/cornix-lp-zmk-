/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NAPE_SCROLL_INERTIA_TICK_MS 16
#define NAPE_SCROLL_INERTIA_START_DELAY_MS 40
#define NAPE_SCROLL_INERTIA_DECAY_NUMERATOR 230
#define NAPE_SCROLL_INERTIA_DECAY_DENOMINATOR 256
#define NAPE_SCROLL_INERTIA_MIN_START_COUNTS 8
#define NAPE_SCROLL_INERTIA_STOP_COUNTS 1
#define NAPE_SCROLL_INERTIA_MAX_DURATION_MS 1200
#define NAPE_SCROLL_INERTIA_VELOCITY_SCALE 256
#define NAPE_SCROLL_INERTIA_MAX_COUNTS_PER_TICK 64
#define NAPE_SCROLL_INERTIA_MAX_SAMPLE_GAP_MS 80
#define NAPE_SCROLL_INERTIA_MAX_FRAME_DELTA 127
#define NAPE_SCROLL_INERTIA_EVENT_EPOCH_MASK 0x7fff

struct nape_inertia_axis {
    int32_t velocity_q8;
    int32_t remainder_q8;
};

static inline int32_t nape_inertia_clamp_velocity(int32_t velocity_q8) {
    const int32_t limit = NAPE_SCROLL_INERTIA_MAX_COUNTS_PER_TICK *
                          NAPE_SCROLL_INERTIA_VELOCITY_SCALE;
    if (velocity_q8 > limit) {
        return limit;
    }
    if (velocity_q8 < -limit) {
        return -limit;
    }
    return velocity_q8;
}

static inline int32_t nape_inertia_sample_velocity(int32_t delta, uint32_t elapsed_ms,
                                                   bool has_previous_sample) {
    if (delta > NAPE_SCROLL_INERTIA_MAX_FRAME_DELTA) {
        delta = NAPE_SCROLL_INERTIA_MAX_FRAME_DELTA;
    } else if (delta < -NAPE_SCROLL_INERTIA_MAX_FRAME_DELTA) {
        delta = -NAPE_SCROLL_INERTIA_MAX_FRAME_DELTA;
    }

    if (!has_previous_sample || elapsed_ms == 0 ||
        elapsed_ms > NAPE_SCROLL_INERTIA_MAX_SAMPLE_GAP_MS) {
        return nape_inertia_clamp_velocity(delta * NAPE_SCROLL_INERTIA_VELOCITY_SCALE);
    }

    const int32_t sample_q8 = delta * NAPE_SCROLL_INERTIA_TICK_MS *
                              NAPE_SCROLL_INERTIA_VELOCITY_SCALE / (int32_t)elapsed_ms;
    return nape_inertia_clamp_velocity(sample_q8);
}

static inline int32_t nape_inertia_update_velocity(int32_t old_velocity_q8,
                                                   int32_t sample_velocity_q8) {
    if (sample_velocity_q8 == 0) {
        return old_velocity_q8 * 3 / 4;
    }
    if ((old_velocity_q8 > 0 && sample_velocity_q8 < 0) ||
        (old_velocity_q8 < 0 && sample_velocity_q8 > 0)) {
        return sample_velocity_q8;
    }
    return nape_inertia_clamp_velocity((old_velocity_q8 * 3 + sample_velocity_q8) / 4);
}

static inline bool nape_inertia_meets_start_threshold(int32_t velocity_q8) {
    const int32_t threshold = NAPE_SCROLL_INERTIA_MIN_START_COUNTS *
                              NAPE_SCROLL_INERTIA_VELOCITY_SCALE;
    return velocity_q8 >= threshold || velocity_q8 <= -threshold;
}

static inline int32_t nape_inertia_axis_step(struct nape_inertia_axis *axis) {
    const int32_t total = axis->remainder_q8 + axis->velocity_q8;
    const int32_t emitted = total / NAPE_SCROLL_INERTIA_VELOCITY_SCALE;
    axis->remainder_q8 = total % NAPE_SCROLL_INERTIA_VELOCITY_SCALE;
    axis->velocity_q8 = axis->velocity_q8 * NAPE_SCROLL_INERTIA_DECAY_NUMERATOR /
                        NAPE_SCROLL_INERTIA_DECAY_DENOMINATOR;

    if (axis->velocity_q8 < NAPE_SCROLL_INERTIA_STOP_COUNTS *
                                NAPE_SCROLL_INERTIA_VELOCITY_SCALE &&
        axis->velocity_q8 > -NAPE_SCROLL_INERTIA_STOP_COUNTS *
                                 NAPE_SCROLL_INERTIA_VELOCITY_SCALE) {
        axis->velocity_q8 = 0;
        axis->remainder_q8 = 0;
    }
    return emitted;
}

static inline bool nape_inertia_duration_expired(uint32_t elapsed_ms) {
    return elapsed_ms >= NAPE_SCROLL_INERTIA_MAX_DURATION_MS;
}

static inline int32_t nape_inertia_pack_scroll_event(uint32_t epoch, int32_t delta) {
    return (int32_t)(((epoch & NAPE_SCROLL_INERTIA_EVENT_EPOCH_MASK) << 16) |
                     (uint16_t)delta);
}

static inline bool nape_inertia_scroll_event_is_current(int32_t payload, uint32_t epoch) {
    return ((uint32_t)payload >> 16) == (epoch & NAPE_SCROLL_INERTIA_EVENT_EPOCH_MASK);
}

static inline int16_t nape_inertia_scroll_event_delta(int32_t payload) {
    const uint16_t encoded = (uint16_t)payload;
    if (encoded <= INT16_MAX) {
        return (int16_t)encoded;
    }
    return (int16_t)((int32_t)encoded - 65536);
}

static inline void nape_inertia_axis_reset(struct nape_inertia_axis *axis) {
    axis->velocity_q8 = 0;
    axis->remainder_q8 = 0;
}
