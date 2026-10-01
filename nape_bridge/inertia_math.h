/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NAPE_CURSOR_ACCEL_START_COUNTS 2
#define NAPE_CURSOR_ACCEL_FULL_COUNTS 12
#define NAPE_CURSOR_ACCEL_BASE_Q8 256
#define NAPE_CURSOR_ACCEL_MAX_Q8 358

#define NAPE_CURSOR_INERTIA_TICK_MS 16
#define NAPE_CURSOR_INERTIA_START_DELAY_MS 48
#define NAPE_CURSOR_INERTIA_DECAY_NUMERATOR 192
#define NAPE_CURSOR_INERTIA_DECAY_DENOMINATOR 256
#define NAPE_CURSOR_INERTIA_MIN_START_COUNTS_PER_TICK 12
#define NAPE_CURSOR_INERTIA_MIN_REPORTS 2
#define NAPE_CURSOR_INERTIA_MIN_RAW_COUNTS 24
#define NAPE_CURSOR_INERTIA_SECONDARY_NUMERATOR 1
#define NAPE_CURSOR_INERTIA_SECONDARY_DENOMINATOR 4
#define NAPE_CURSOR_INERTIA_STOP_VELOCITY_Q8 128
#define NAPE_CURSOR_INERTIA_MAX_DURATION_MS 224
#define NAPE_CURSOR_INERTIA_MAX_IDLE_MS 350
#define NAPE_CURSOR_INERTIA_SAMPLE_WINDOW_MS 80
#define NAPE_CURSOR_INERTIA_VELOCITY_SCALE 256
#define NAPE_CURSOR_INERTIA_MAX_COUNTS_PER_TICK 64
#define NAPE_CURSOR_INERTIA_MAX_SAMPLE_GAP_MS 80
#define NAPE_CURSOR_INERTIA_MAX_FRAME_DELTA 160
#define NAPE_CURSOR_INERTIA_HISTORY_CAPACITY 12
#define NAPE_CURSOR_INERTIA_EVENT_EPOCH_MASK 0x7fff

struct nape_inertia_axis {
    int32_t velocity_q8;
    int32_t remainder_q8;
};

struct nape_cursor_accel_axis {
    int32_t remainder_q8;
    int8_t direction;
};

struct nape_cursor_motion_sample {
    uint32_t received_ms;
    uint16_t raw_counts;
};

struct nape_cursor_motion_history {
    struct nape_cursor_motion_sample samples[NAPE_CURSOR_INERTIA_HISTORY_CAPACITY];
    uint32_t raw_total;
    uint8_t head;
    uint8_t count;
};

static inline uint64_t nape_cursor_magnitude(int32_t value) {
    return value < 0 ? (uint64_t)(-(int64_t)value) : (uint64_t)value;
}

static inline uint32_t nape_cursor_accel_gain_q8(uint32_t magnitude) {
    if (magnitude <= NAPE_CURSOR_ACCEL_START_COUNTS) {
        return NAPE_CURSOR_ACCEL_BASE_Q8;
    }
    if (magnitude >= NAPE_CURSOR_ACCEL_FULL_COUNTS) {
        return NAPE_CURSOR_ACCEL_MAX_Q8;
    }

    const uint32_t progress = magnitude - NAPE_CURSOR_ACCEL_START_COUNTS;
    const uint32_t range = NAPE_CURSOR_ACCEL_FULL_COUNTS - NAPE_CURSOR_ACCEL_START_COUNTS;
    const uint32_t gain_range = NAPE_CURSOR_ACCEL_MAX_Q8 - NAPE_CURSOR_ACCEL_BASE_Q8;
    return NAPE_CURSOR_ACCEL_BASE_Q8 + (gain_range * progress) / range;
}

static inline void nape_cursor_accel_axis_reset(struct nape_cursor_accel_axis *axis) {
    axis->remainder_q8 = 0;
    axis->direction = 0;
}

static inline int32_t nape_cursor_accel_scale(int32_t value,
                                              struct nape_cursor_accel_axis *axis) {
    if (value == 0) {
        return 0;
    }

    const int8_t direction = value < 0 ? -1 : 1;
    if (axis->direction != 0 && axis->direction != direction) {
        axis->remainder_q8 = 0;
    }
    axis->direction = direction;

    const uint64_t magnitude = nape_cursor_magnitude(value);
    const uint32_t gain_q8 = nape_cursor_accel_gain_q8(
        magnitude > UINT32_MAX ? UINT32_MAX : (uint32_t)magnitude);
    const int64_t numerator = (int64_t)value * gain_q8 + axis->remainder_q8;
    int64_t scaled = numerator / NAPE_CURSOR_ACCEL_BASE_Q8;

    if (scaled > INT32_MAX) {
        scaled = INT32_MAX;
        axis->remainder_q8 = 0;
    } else if (scaled < INT32_MIN) {
        scaled = INT32_MIN;
        axis->remainder_q8 = 0;
    } else {
        axis->remainder_q8 = (int32_t)(numerator -
                                      scaled * NAPE_CURSOR_ACCEL_BASE_Q8);
    }

    return (int32_t)scaled;
}

static inline int32_t nape_cursor_inertia_clamp_velocity(int32_t velocity_q8) {
    const int32_t limit = NAPE_CURSOR_INERTIA_MAX_COUNTS_PER_TICK *
                          NAPE_CURSOR_INERTIA_VELOCITY_SCALE;
    if (velocity_q8 > limit) {
        return limit;
    }
    if (velocity_q8 < -limit) {
        return -limit;
    }
    return velocity_q8;
}

static inline int32_t nape_cursor_inertia_sample_velocity(int32_t delta, uint32_t elapsed_ms,
                                                          bool has_previous_sample) {
    if (delta > NAPE_CURSOR_INERTIA_MAX_FRAME_DELTA) {
        delta = NAPE_CURSOR_INERTIA_MAX_FRAME_DELTA;
    } else if (delta < -NAPE_CURSOR_INERTIA_MAX_FRAME_DELTA) {
        delta = -NAPE_CURSOR_INERTIA_MAX_FRAME_DELTA;
    }

    if (!has_previous_sample || elapsed_ms == 0 ||
        elapsed_ms > NAPE_CURSOR_INERTIA_MAX_SAMPLE_GAP_MS) {
        return nape_cursor_inertia_clamp_velocity(
            delta * NAPE_CURSOR_INERTIA_VELOCITY_SCALE);
    }

    const int32_t sample_q8 = delta * NAPE_CURSOR_INERTIA_TICK_MS *
                              NAPE_CURSOR_INERTIA_VELOCITY_SCALE / (int32_t)elapsed_ms;
    return nape_cursor_inertia_clamp_velocity(sample_q8);
}

static inline int32_t nape_cursor_inertia_update_velocity(int32_t old_velocity_q8,
                                                          int32_t sample_velocity_q8) {
    if (sample_velocity_q8 == 0) {
        return old_velocity_q8 * 3 / 4;
    }
    if ((old_velocity_q8 > 0 && sample_velocity_q8 < 0) ||
        (old_velocity_q8 < 0 && sample_velocity_q8 > 0)) {
        return sample_velocity_q8;
    }
    return nape_cursor_inertia_clamp_velocity(
        (old_velocity_q8 * 3 + sample_velocity_q8) / 4);
}

static inline void nape_cursor_inertia_history_clear(
    struct nape_cursor_motion_history *history) {
    history->raw_total = 0;
    history->head = 0;
    history->count = 0;
}

static inline void nape_cursor_inertia_history_remove_oldest(
    struct nape_cursor_motion_history *history) {
    if (history->count == 0) {
        return;
    }

    const uint16_t raw_counts = history->samples[history->head].raw_counts;
    history->raw_total = history->raw_total >= raw_counts
                             ? history->raw_total - raw_counts
                             : 0;
    history->head = (uint8_t)((history->head + 1u) %
                              NAPE_CURSOR_INERTIA_HISTORY_CAPACITY);
    history->count--;
}

static inline void nape_cursor_inertia_history_prune(
    struct nape_cursor_motion_history *history, uint32_t now_ms) {
    while (history->count != 0) {
        const uint32_t age_ms =
            now_ms - history->samples[history->head].received_ms;
        if (age_ms < NAPE_CURSOR_INERTIA_SAMPLE_WINDOW_MS) {
            break;
        }
        nape_cursor_inertia_history_remove_oldest(history);
    }
}

static inline void nape_cursor_inertia_history_add(
    struct nape_cursor_motion_history *history, int32_t raw_x, int32_t raw_y,
    uint32_t received_ms) {
    const uint64_t raw_total = nape_cursor_magnitude(raw_x) +
                               nape_cursor_magnitude(raw_y);
    if (raw_total == 0) {
        return;
    }

    const uint16_t raw_counts = raw_total > UINT16_MAX
                                    ? UINT16_MAX
                                    : (uint16_t)raw_total;
    nape_cursor_inertia_history_prune(history, received_ms);
    if (history->count == NAPE_CURSOR_INERTIA_HISTORY_CAPACITY) {
        nape_cursor_inertia_history_remove_oldest(history);
    }

    const uint8_t tail = (uint8_t)((history->head + history->count) %
                                   NAPE_CURSOR_INERTIA_HISTORY_CAPACITY);
    history->samples[tail].received_ms = received_ms;
    history->samples[tail].raw_counts = raw_counts;
    history->raw_total += raw_counts;
    history->count++;
}

static inline bool nape_cursor_inertia_meets_start_threshold(int32_t velocity_q8) {
    const int32_t threshold = NAPE_CURSOR_INERTIA_MIN_START_COUNTS_PER_TICK *
                              NAPE_CURSOR_INERTIA_VELOCITY_SCALE;
    return velocity_q8 >= threshold || velocity_q8 <= -threshold;
}

static inline bool nape_cursor_inertia_should_start(
    const struct nape_cursor_motion_history *history, int32_t velocity_x_q8,
    int32_t velocity_y_q8) {
    if (history->count < NAPE_CURSOR_INERTIA_MIN_REPORTS ||
        history->raw_total < NAPE_CURSOR_INERTIA_MIN_RAW_COUNTS) {
        return false;
    }
    return nape_cursor_inertia_meets_start_threshold(velocity_x_q8) ||
           nape_cursor_inertia_meets_start_threshold(velocity_y_q8);
}

static inline bool nape_cursor_inertia_keep_axis(int32_t velocity_q8,
                                                  uint64_t primary_magnitude_q8) {
    return primary_magnitude_q8 != 0 &&
           nape_cursor_magnitude(velocity_q8) *
                   NAPE_CURSOR_INERTIA_SECONDARY_DENOMINATOR >=
               primary_magnitude_q8 * NAPE_CURSOR_INERTIA_SECONDARY_NUMERATOR;
}

static inline int32_t nape_cursor_inertia_seed_velocity(int32_t velocity_q8) {
    return velocity_q8 / 4;
}

static inline int32_t nape_cursor_inertia_axis_step(struct nape_inertia_axis *axis) {
    const int32_t total = axis->remainder_q8 + axis->velocity_q8;
    const int32_t emitted = total / NAPE_CURSOR_INERTIA_VELOCITY_SCALE;
    axis->remainder_q8 = total % NAPE_CURSOR_INERTIA_VELOCITY_SCALE;
    axis->velocity_q8 =
        axis->velocity_q8 * NAPE_CURSOR_INERTIA_DECAY_NUMERATOR /
        NAPE_CURSOR_INERTIA_DECAY_DENOMINATOR;

    if (axis->velocity_q8 < NAPE_CURSOR_INERTIA_STOP_VELOCITY_Q8 &&
        axis->velocity_q8 > -NAPE_CURSOR_INERTIA_STOP_VELOCITY_Q8) {
        axis->velocity_q8 = 0;
        axis->remainder_q8 = 0;
    }
    return emitted;
}

static inline bool nape_cursor_inertia_duration_expired(uint32_t run_ms,
                                                         uint32_t idle_ms) {
    return run_ms >= NAPE_CURSOR_INERTIA_MAX_DURATION_MS ||
           idle_ms >= NAPE_CURSOR_INERTIA_MAX_IDLE_MS;
}

static inline bool nape_cursor_inertia_event_is_current(int32_t payload,
                                                         uint32_t epoch) {
    return ((uint32_t)payload >> 16) ==
           (epoch & NAPE_CURSOR_INERTIA_EVENT_EPOCH_MASK);
}

static inline int32_t nape_cursor_inertia_pack_event(uint32_t epoch, int32_t delta) {
    return (int32_t)(((epoch & NAPE_CURSOR_INERTIA_EVENT_EPOCH_MASK) << 16) |
                     (uint16_t)delta);
}

static inline int16_t nape_cursor_inertia_event_delta(int32_t payload) {
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
