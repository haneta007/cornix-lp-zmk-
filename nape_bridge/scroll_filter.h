/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NAPE_SCROLL_X_AXIS_DOMINANCE_RATIO 2
#define NAPE_SCROLL_X_AXIS_THRESHOLD_COUNTS 4
#define NAPE_SCROLL_VERTICAL_AXIS_LOCK_MS 160
#define NAPE_SCROLL_X_ACCUMULATOR_GAP_MS 80
#define NAPE_SCROLL_X_CONFIRM_COUNTS 16
#define NAPE_SCROLL_X_CONFIRM_REPORTS 2
#define NAPE_SCROLL_GESTURE_IDLE_MS 160

struct nape_scroll_axis_filter {
    int32_t pending_x;
    uint32_t last_vertical_ms;
    uint32_t last_pending_ms;
    uint32_t last_motion_ms;
    bool vertical_lock;
    bool pending_active;
    uint8_t confirmation_reports;
    bool horizontal_confirmed;
    bool motion_active;
};

struct nape_scroll_motion {
    int32_t scroll_x;
    int32_t scroll_y;
    int32_t velocity_x;
    int32_t velocity_y;
};

static inline uint64_t nape_scroll_axis_magnitude(int32_t value) {
    return value < 0 ? (uint64_t)(-(int64_t)value) : (uint64_t)value;
}

static inline int32_t nape_scroll_axis_negate(int32_t value) {
    return value == INT32_MIN ? INT32_MAX : -value;
}

static inline void nape_scroll_axis_filter_reset(struct nape_scroll_axis_filter *state) {
    state->pending_x = 0;
    state->last_vertical_ms = 0;
    state->last_pending_ms = 0;
    state->vertical_lock = false;
    state->last_motion_ms = 0;
    state->horizontal_confirmed = false;
    state->motion_active = false;
    state->pending_active = false;
    state->confirmation_reports = 0;
}

static inline void nape_scroll_axis_filter_clear_pending(struct nape_scroll_axis_filter *state) {
    state->pending_x = 0;
    state->last_pending_ms = 0;
    state->pending_active = false;
    state->confirmation_reports = 0;
}

static inline int32_t nape_scroll_axis_filter_clamp(int64_t value) {
    if (value > INT32_MAX) {
        return INT32_MAX;
    }
    if (value < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)value;
}

/* Keep the vertical axis direct, and admit horizontal movement only when it is deliberate. */
static inline void nape_scroll_axis_filter_process(struct nape_scroll_axis_filter *state,
                                                   int32_t raw_x, int32_t raw_y,
                                                   uint32_t received_ms,
                                                   struct nape_scroll_motion *motion) {
    motion->scroll_x = 0;
    motion->scroll_y = nape_scroll_axis_negate(raw_y);
    motion->velocity_x = 0;
    motion->velocity_y = motion->scroll_y;

    /* Zero reports do not extend the gesture or count as confirmation. */
    if (raw_x == 0 && raw_y == 0) {
        return;
    }
    if (state->motion_active &&
        (uint32_t)(received_ms - state->last_motion_ms) >= NAPE_SCROLL_GESTURE_IDLE_MS) {
        nape_scroll_axis_filter_reset(state);
    }
    state->last_motion_ms = received_ms;
    state->motion_active = true;

    const uint64_t x_magnitude = nape_scroll_axis_magnitude(raw_x);
    const uint64_t y_magnitude = nape_scroll_axis_magnitude(raw_y);
    const bool x_dominant = x_magnitude != 0 &&
                            x_magnitude >= y_magnitude * NAPE_SCROLL_X_AXIS_DOMINANCE_RATIO;
    const bool strong_horizontal =
        x_dominant && x_magnitude >= NAPE_SCROLL_X_AXIS_THRESHOLD_COUNTS;

    if (y_magnitude != 0 && !strong_horizontal) {
        state->horizontal_confirmed = false;
        state->vertical_lock = true;
        state->last_vertical_ms = received_ms;
        nape_scroll_axis_filter_clear_pending(state);
    } else if (state->vertical_lock &&
               (uint32_t)(received_ms - state->last_vertical_ms) >=
                   NAPE_SCROLL_VERTICAL_AXIS_LOCK_MS) {
        state->vertical_lock = false;
    }

    if (!state->horizontal_confirmed) {
        /* A single release/startup twitch must never unlock horizontal scrolling. */
        if (!strong_horizontal) {
            nape_scroll_axis_filter_clear_pending(state);
            return;
        }
        if (state->pending_active &&
            ((uint32_t)(received_ms - state->last_pending_ms) >
                 NAPE_SCROLL_X_ACCUMULATOR_GAP_MS ||
             (state->pending_x < 0 && raw_x > 0) ||
             (state->pending_x > 0 && raw_x < 0))) {
            nape_scroll_axis_filter_clear_pending(state);
        }
        state->pending_x = nape_scroll_axis_filter_clamp((int64_t)state->pending_x + raw_x);
        state->last_pending_ms = received_ms;
        state->pending_active = true;
        if (state->confirmation_reports < NAPE_SCROLL_X_CONFIRM_REPORTS) {
            state->confirmation_reports++;
        }
        if (state->confirmation_reports < NAPE_SCROLL_X_CONFIRM_REPORTS ||
            nape_scroll_axis_magnitude(state->pending_x) < NAPE_SCROLL_X_CONFIRM_COUNTS) {
            return;
        }
        state->horizontal_confirmed = true;
        state->vertical_lock = false;
        nape_scroll_axis_filter_clear_pending(state);
        /* Discard withheld samples rather than replaying them as a jump. */
        motion->scroll_x = raw_x;
        motion->velocity_x = raw_x;
        return;
    }

    if (strong_horizontal) {
        motion->scroll_x = raw_x;
        motion->velocity_x = raw_x;
        nape_scroll_axis_filter_clear_pending(state);
        return;
    }
    if (!x_dominant || state->vertical_lock) {
        nape_scroll_axis_filter_clear_pending(state);
        return;
    }

    motion->velocity_x = raw_x;
    if (state->pending_active &&
        ((uint32_t)(received_ms - state->last_pending_ms) >
             NAPE_SCROLL_X_ACCUMULATOR_GAP_MS ||
         (state->pending_x < 0 && raw_x > 0) ||
         (state->pending_x > 0 && raw_x < 0))) {
        nape_scroll_axis_filter_clear_pending(state);
    }

    const int64_t accumulated = (int64_t)state->pending_x + raw_x;
    state->last_pending_ms = received_ms;
    if (nape_scroll_axis_magnitude(nape_scroll_axis_filter_clamp(accumulated)) >=
        NAPE_SCROLL_X_AXIS_THRESHOLD_COUNTS) {
        motion->scroll_x = nape_scroll_axis_filter_clamp(accumulated);
        nape_scroll_axis_filter_clear_pending(state);
    } else {
        state->pending_x = (int32_t)accumulated;
        state->pending_active = true;
    }
}
