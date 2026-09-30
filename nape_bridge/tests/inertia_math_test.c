/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../inertia_math.h"
#include "../scroll_filter.h"

static void test_velocity_sampling_and_threshold(void) {
    int32_t slow = nape_inertia_sample_velocity(3, 16, true);
    int32_t fast = nape_inertia_sample_velocity(16, 16, true);
    assert(!nape_inertia_meets_start_threshold(slow));
    assert(nape_inertia_meets_start_threshold(fast));
    assert(nape_inertia_meets_start_threshold(-fast));
    assert(nape_inertia_sample_velocity(127, 16, true) ==
           NAPE_SCROLL_INERTIA_MAX_COUNTS_PER_TICK * NAPE_SCROLL_INERTIA_VELOCITY_SCALE);
}

static void test_velocity_uses_elapsed_report_time(void) {
    const int32_t movement = 8 * NAPE_SCROLL_INERTIA_VELOCITY_SCALE;

    assert(nape_inertia_sample_velocity(8, 16, true) == movement);
    assert(nape_inertia_sample_velocity(8, 32, true) == movement / 2);
    assert(nape_inertia_sample_velocity(8, 0, true) == movement);
    assert(nape_inertia_sample_velocity(8, NAPE_SCROLL_INERTIA_MAX_SAMPLE_GAP_MS + 1, true) ==
           movement);
}

static void test_velocity_smoothing_and_reversal(void) {
    const int32_t sample = 16 * NAPE_SCROLL_INERTIA_VELOCITY_SCALE;
    assert(nape_inertia_update_velocity(0, sample) == sample / 4);
    assert(nape_inertia_update_velocity(sample, -sample) == -sample);
    assert(nape_inertia_update_velocity(sample, 0) == sample * 3 / 4);
}

static void test_positive_and_negative_decay(void) {
    struct nape_inertia_axis positive = {.velocity_q8 = 8 * 256};
    struct nape_inertia_axis negative = {.velocity_q8 = -8 * 256};
    int32_t previous_positive = positive.velocity_q8;
    int32_t previous_negative = negative.velocity_q8;
    (void)nape_inertia_axis_step(&positive);
    (void)nape_inertia_axis_step(&negative);
    assert(positive.velocity_q8 > 0 && positive.velocity_q8 < previous_positive);
    assert(negative.velocity_q8 < 0 && negative.velocity_q8 > previous_negative);
}

static void test_fractional_accumulation_and_stop(void) {
    struct nape_inertia_axis axis = {.velocity_q8 = 384};
    assert(nape_inertia_axis_step(&axis) == 1);
    assert(axis.remainder_q8 > 0);
    assert(nape_inertia_axis_step(&axis) == 1);

    axis.velocity_q8 = 256;
    axis.remainder_q8 = 100;
    assert(nape_inertia_axis_step(&axis) == 1);
    assert(axis.velocity_q8 == 0);
    assert(axis.remainder_q8 == 0);
}

static void test_max_duration_and_reset(void) {
    struct nape_inertia_axis axis = {.velocity_q8 = 12 * 256, .remainder_q8 = 42};
    assert(!nape_inertia_duration_expired(1199));
    assert(nape_inertia_duration_expired(1200));
    nape_inertia_axis_reset(&axis);
    assert(axis.velocity_q8 == 0 && axis.remainder_q8 == 0);
}

static void test_vertical_lock_suppresses_horizontal_jitter(void) {
    struct nape_scroll_axis_filter state = {0};
    struct nape_scroll_motion motion;

    nape_scroll_axis_filter_process(&state, 2, 8, 1000, &motion);
    assert(motion.scroll_x == 0 && motion.velocity_x == 0);
    assert(motion.scroll_y == -8 && motion.velocity_y == -8);
    nape_scroll_axis_filter_process(&state, -1, 7, 1016, &motion);
    assert(motion.scroll_x == 0 && motion.velocity_x == 0);
    assert(motion.scroll_y == -7);
}

static void test_vertical_component_keeps_lock_for_weak_x(void) {
    struct nape_scroll_axis_filter state = {0};
    struct nape_scroll_motion motion;

    nape_scroll_axis_filter_process(&state, 0, 8, 1000, &motion);
    nape_scroll_axis_filter_process(&state, 2, 1, 1016, &motion);
    assert(state.vertical_lock);
    assert(motion.scroll_x == 0 && motion.velocity_x == 0);
    nape_scroll_axis_filter_process(&state, 1, 0, 1070, &motion);
    assert(motion.scroll_x == 0 && motion.velocity_x == 0);
}

static void test_clear_horizontal_motion_switches_immediately(void) {
    struct nape_scroll_axis_filter state = {0};
    struct nape_scroll_motion motion;

    nape_scroll_axis_filter_process(&state, 0, 8, 1000, &motion);
    nape_scroll_axis_filter_process(&state, 4, 2, 1016, &motion);
    assert(motion.scroll_x == 4 && motion.velocity_x == 4);
    assert(!state.vertical_lock);
}

static void test_slow_horizontal_motion_accumulates_after_vertical_lock(void) {
    struct nape_scroll_axis_filter state = {0};
    struct nape_scroll_motion motion;

    nape_scroll_axis_filter_process(&state, 0, 8, 1000, &motion);
    nape_scroll_axis_filter_process(&state, 1, 0, 1040, &motion);
    assert(motion.scroll_x == 0 && motion.velocity_x == 0);
    nape_scroll_axis_filter_process(&state, 1, 0, 1065, &motion);
    assert(motion.scroll_x == 0 && motion.velocity_x == 1);
    nape_scroll_axis_filter_process(&state, 1, 0, 1081, &motion);
    assert(motion.scroll_x == 0);
    nape_scroll_axis_filter_process(&state, 1, 0, 1097, &motion);
    assert(motion.scroll_x == 0);
    nape_scroll_axis_filter_process(&state, 1, 0, 1113, &motion);
    assert(motion.scroll_x == 4);
    assert(motion.scroll_y == 0);
}

static void test_vertical_input_discards_pending_horizontal_counts(void) {
    struct nape_scroll_axis_filter state = {0};
    struct nape_scroll_motion motion;

    nape_scroll_axis_filter_process(&state, 1, 0, 1000, &motion);
    nape_scroll_axis_filter_process(&state, 1, 0, 1016, &motion);
    assert(state.pending_x == 2);
    nape_scroll_axis_filter_process(&state, 0, 1, 1032, &motion);
    assert(state.pending_x == 0);
    assert(motion.scroll_y == -1);
}

static void test_horizontal_reversal_and_idle_gap_reset_accumulator(void) {
    struct nape_scroll_axis_filter state = {0};
    struct nape_scroll_motion motion;

    nape_scroll_axis_filter_process(&state, 2, 0, 1000, &motion);
    nape_scroll_axis_filter_process(&state, -2, 0, 1016, &motion);
    assert(state.pending_x == -2);
    nape_scroll_axis_filter_process(&state, -2, 0, 1032, &motion);
    assert(motion.scroll_x == -4);

    nape_scroll_axis_filter_process(&state, 2, 0, 1100, &motion);
    nape_scroll_axis_filter_process(&state, 2, 0, 1200, &motion);
    assert(state.pending_x == 2);
    assert(motion.scroll_x == 0);
}

static void test_scroll_direction_and_filter_reset(void) {
    struct nape_scroll_axis_filter state = {0};
    struct nape_scroll_motion motion;

    nape_scroll_axis_filter_process(&state, 0, 5, 1000, &motion);
    assert(motion.scroll_y == -5 && motion.velocity_y == -5);
    nape_scroll_axis_filter_process(&state, 0, INT32_MIN, 1016, &motion);
    assert(motion.scroll_y == INT32_MAX);
    nape_scroll_axis_filter_process(&state, 1, 0, 1081, &motion);
    assert(state.pending_active);
    nape_scroll_axis_filter_reset(&state);
    assert(state.pending_x == 0 && !state.pending_active && !state.vertical_lock);
}

static void test_queued_scroll_epoch_and_signed_delta(void) {
    const int32_t positive = nape_inertia_pack_scroll_event(42, 7);
    const int32_t negative = nape_inertia_pack_scroll_event(42, -3);

    assert(nape_inertia_scroll_event_is_current(positive, 42));
    assert(nape_inertia_scroll_event_delta(positive) == 7);
    assert(nape_inertia_scroll_event_is_current(negative, 42));
    assert(nape_inertia_scroll_event_delta(negative) == -3);
    assert(!nape_inertia_scroll_event_is_current(positive, 43));
    assert(nape_inertia_scroll_event_is_current(
        nape_inertia_pack_scroll_event(NAPE_SCROLL_INERTIA_EVENT_EPOCH_MASK + 1, 1), 0));
}

int main(void) {
    test_velocity_sampling_and_threshold();
    test_velocity_uses_elapsed_report_time();
    test_velocity_smoothing_and_reversal();
    test_positive_and_negative_decay();
    test_fractional_accumulation_and_stop();
    test_max_duration_and_reset();
    test_vertical_lock_suppresses_horizontal_jitter();
    test_vertical_component_keeps_lock_for_weak_x();
    test_clear_horizontal_motion_switches_immediately();
    test_slow_horizontal_motion_accumulates_after_vertical_lock();
    test_vertical_input_discards_pending_horizontal_counts();
    test_horizontal_reversal_and_idle_gap_reset_accumulator();
    test_scroll_direction_and_filter_reset();
    test_queued_scroll_epoch_and_signed_delta();
    puts("Nape inertia math tests: PASS");
    return 0;
}
