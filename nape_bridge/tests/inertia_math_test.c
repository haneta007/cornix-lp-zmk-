/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../inertia_math.h"
#include "../scroll_filter.h"

static void test_acceleration_profile_and_direction(void) {
    struct nape_cursor_accel_axis axis = {0};

    assert(nape_cursor_accel_gain_q8(2) == 256);
    assert(nape_cursor_accel_gain_q8(7) == 307);
    assert(nape_cursor_accel_gain_q8(12) == 358);
    assert(nape_cursor_accel_gain_q8(100) == 358);
    assert(nape_cursor_accel_scale(2, &axis) == 2);
    assert(nape_cursor_accel_scale(8, &axis) == 9);

    nape_cursor_accel_axis_reset(&axis);
    assert(nape_cursor_accel_scale(12, &axis) == 16);
    nape_cursor_accel_axis_reset(&axis);
    assert(nape_cursor_accel_scale(20, &axis) == 27);
    nape_cursor_accel_axis_reset(&axis);
    assert(nape_cursor_accel_scale(-12, &axis) == -16);
}

static void test_acceleration_fraction_and_reversal(void) {
    struct nape_cursor_accel_axis axis = {0};

    assert(nape_cursor_accel_scale(3, &axis) == 3);
    assert(axis.remainder_q8 > 0);
    const int32_t previous_remainder = axis.remainder_q8;
    assert(nape_cursor_accel_scale(3, &axis) == 3);
    assert(axis.remainder_q8 > previous_remainder);

    assert(nape_cursor_accel_scale(-3, &axis) == -3);
    assert(axis.remainder_q8 < 0);
    assert(nape_cursor_accel_scale(3, &axis) == 3);
    assert(axis.remainder_q8 > 0);
}

static void test_velocity_sampling_and_direction_reversal(void) {
    const int32_t below = nape_cursor_inertia_sample_velocity(11, 16, true);
    const int32_t threshold = nape_cursor_inertia_sample_velocity(12, 16, true);
    const int32_t negative = nape_cursor_inertia_sample_velocity(-12, 16, true);

    assert(!nape_cursor_inertia_meets_start_threshold(below));
    assert(nape_cursor_inertia_meets_start_threshold(threshold));
    assert(nape_cursor_inertia_meets_start_threshold(negative));
    assert(nape_cursor_inertia_sample_velocity(8, 16, true) == 8 * 256);
    assert(nape_cursor_inertia_sample_velocity(8, 32, true) == 4 * 256);
    assert(nape_cursor_inertia_sample_velocity(8, 81, true) == 8 * 256);
    assert(nape_cursor_inertia_sample_velocity(160, 16, true) == 64 * 256);

    assert(nape_cursor_inertia_update_velocity(0, 16 * 256) == 4 * 256);
    assert(nape_cursor_inertia_update_velocity(16 * 256, -16 * 256) == -16 * 256);
    assert(nape_cursor_inertia_update_velocity(16 * 256, 0) == 12 * 256);
}

static void test_recent_movement_gate_and_expiry(void) {
    struct nape_cursor_motion_history history = {0};

    nape_cursor_inertia_history_add(&history, 12, 0, 1000);
    assert(!nape_cursor_inertia_should_start(&history, 20 * 256, 0));
    nape_cursor_inertia_history_add(&history, 12, 0, 1040);
    assert(nape_cursor_inertia_should_start(&history, 12 * 256, 0));
    assert(!nape_cursor_inertia_should_start(&history, 11 * 256, 0));

    nape_cursor_inertia_history_prune(&history, 1040);
    assert(history.count == 2);
    nape_cursor_inertia_history_prune(&history, 1080);
    assert(history.count == 1);
    assert(!nape_cursor_inertia_should_start(&history, 20 * 256, 0));

    nape_cursor_inertia_history_clear(&history);
    assert(history.count == 0 && history.raw_total == 0);
    nape_cursor_inertia_history_add(&history, 3, 0, 2000);
    nape_cursor_inertia_history_add(&history, 3, 0, 2016);
    assert(!nape_cursor_inertia_should_start(&history, 20 * 256, 0));
    nape_cursor_inertia_history_clear(&history);
    nape_cursor_inertia_history_add(&history, 24, 0, 2100);
    assert(!nape_cursor_inertia_should_start(&history, 20 * 256, 0));
}

static void test_secondary_axis_and_seed(void) {
    const uint64_t primary = 16u * 256u;
    assert(nape_cursor_inertia_keep_axis(16 * 256, primary));
    assert(!nape_cursor_inertia_keep_axis(3 * 256, primary));
    assert(nape_cursor_inertia_keep_axis(4 * 256, primary));
    assert(nape_cursor_inertia_keep_axis(-4 * 256, primary));
    assert(nape_cursor_inertia_seed_velocity(16 * 256) == 4 * 256);
}

static void test_positive_negative_decay_and_fractional_stop(void) {
    struct nape_inertia_axis positive = {.velocity_q8 = 8 * 256};
    struct nape_inertia_axis negative = {.velocity_q8 = -8 * 256};
    const int32_t old_positive = positive.velocity_q8;
    const int32_t old_negative = negative.velocity_q8;
    (void)nape_cursor_inertia_axis_step(&positive);
    (void)nape_cursor_inertia_axis_step(&negative);
    assert(positive.velocity_q8 > 0 && positive.velocity_q8 < old_positive);
    assert(negative.velocity_q8 < 0 && negative.velocity_q8 > old_negative);

    struct nape_inertia_axis fractional = {.velocity_q8 = 384};
    assert(nape_cursor_inertia_axis_step(&fractional) == 1);
    assert(fractional.remainder_q8 > 0);
    assert(nape_cursor_inertia_axis_step(&fractional) == 1);

    struct nape_inertia_axis stop = {.velocity_q8 = 160, .remainder_q8 = 20};
    assert(nape_cursor_inertia_axis_step(&stop) == 0);
    assert(stop.velocity_q8 == 0 && stop.remainder_q8 == 0);
}

static void test_duration_idle_and_epoch_reset(void) {
    assert(!nape_cursor_inertia_duration_expired(223, 349));
    assert(nape_cursor_inertia_duration_expired(224, 100));
    assert(nape_cursor_inertia_duration_expired(100, 350));

    struct nape_inertia_axis axis = {.velocity_q8 = 12 * 256, .remainder_q8 = 42};
    nape_inertia_axis_reset(&axis);
    assert(axis.velocity_q8 == 0 && axis.remainder_q8 == 0);

    const int32_t packet = nape_cursor_inertia_pack_event(42, -3);
    assert(nape_cursor_inertia_event_is_current(packet, 42));
    assert(nape_cursor_inertia_event_delta(packet) == -3);
    assert(!nape_cursor_inertia_event_is_current(packet, 43));
}

static void test_scroll_start_confirmation_and_idle(void) {
    struct nape_scroll_axis_filter state = {0};
    struct nape_scroll_motion motion;
    nape_scroll_axis_filter_process(&state, 24, 0, 1000, &motion);
    assert(motion.scroll_x == 0 && !state.horizontal_confirmed);
    nape_scroll_axis_filter_process(&state, 0, 8, 1016, &motion);
    assert(motion.scroll_x == 0 && motion.scroll_y == -8 && state.pending_x == 0);
    nape_scroll_axis_filter_process(&state, 8, 0, 1200, &motion);
    assert(motion.scroll_x == 0);
    nape_scroll_axis_filter_process(&state, 8, 0, 1216, &motion);
    assert(motion.scroll_x == 8 && state.horizontal_confirmed);
    nape_scroll_axis_filter_process(&state, 1, 0, 1232, &motion);
    assert(motion.scroll_x == 0 && state.pending_x == 1);
    nape_scroll_axis_filter_process(&state, 3, 0, 1248, &motion);
    assert(motion.scroll_x == 4);
    nape_scroll_axis_filter_process(&state, 0, 0, 1400, &motion);
    assert(state.last_motion_ms == 1248);
    nape_scroll_axis_filter_process(&state, 24, 0, 1408, &motion);
    assert(motion.scroll_x == 0 && !state.horizontal_confirmed);
}

static void test_scroll_vertical_release_and_switch(void) {
    struct nape_scroll_axis_filter state = {0};
    struct nape_scroll_motion motion;
    nape_scroll_axis_filter_process(&state, 2, 8, 1000, &motion);
    assert(motion.scroll_x == 0 && motion.scroll_y == -8);
    nape_scroll_axis_filter_process(&state, 20, 0, 1016, &motion);
    assert(motion.scroll_x == 0 && state.vertical_lock);
    nape_scroll_axis_filter_process(&state, 1, 0, 1032, &motion);
    assert(motion.scroll_x == 0 && state.pending_x == 0);
    nape_scroll_axis_filter_process(&state, 8, 0, 1048, &motion);
    assert(motion.scroll_x == 0);
    nape_scroll_axis_filter_process(&state, 8, 0, 1064, &motion);
    assert(motion.scroll_x == 8 && !state.vertical_lock);
    nape_scroll_axis_filter_process(&state, 0, -8, 1080, &motion);
    assert(motion.scroll_y == 8 && !state.horizontal_confirmed);
    nape_scroll_axis_filter_process(&state, -20, 0, 1096, &motion);
    assert(motion.scroll_x == 0);
    nape_scroll_axis_filter_reset(&state);
    assert(!state.horizontal_confirmed && !state.motion_active && !state.pending_active);
}

static void test_scroll_confirmation_edges(void) {
    struct nape_scroll_axis_filter state = {0};
    struct nape_scroll_motion motion;
    nape_scroll_axis_filter_process(&state, 3, 0, 1000, &motion);
    nape_scroll_axis_filter_process(&state, 3, 0, 1016, &motion);
    assert(state.pending_x == 0 && motion.scroll_x == 0);
    nape_scroll_axis_filter_process(&state, 4, 0, 1032, &motion);
    nape_scroll_axis_filter_process(&state, 4, 0, 1048, &motion);
    assert(motion.scroll_x == 0);
    nape_scroll_axis_filter_process(&state, 4, 0, 1064, &motion);
    nape_scroll_axis_filter_process(&state, 4, 0, 1080, &motion);
    assert(motion.scroll_x == 4);
    nape_scroll_axis_filter_reset(&state);
    nape_scroll_axis_filter_process(&state, 8, 0, 1100, &motion);
    nape_scroll_axis_filter_process(&state, -8, 0, 1116, &motion);
    assert(motion.scroll_x == 0 && state.pending_x == -8);
    nape_scroll_axis_filter_process(&state, -8, 0, 1197, &motion);
    assert(motion.scroll_x == 0 && state.confirmation_reports == 1);
    nape_scroll_axis_filter_process(&state, -8, 0, 1277, &motion);
    assert(motion.scroll_x == -8);
    nape_scroll_axis_filter_reset(&state);
    nape_scroll_axis_filter_process(&state, 8, 5, 1300, &motion);
    assert(motion.scroll_x == 0 && motion.scroll_y == -5);
    nape_scroll_axis_filter_process(&state, 8, 4, 1316, &motion);
    nape_scroll_axis_filter_process(&state, 8, 4, 1332, &motion);
    assert(motion.scroll_x == 8 && motion.scroll_y == -4);
    nape_scroll_axis_filter_reset(&state);
    nape_scroll_axis_filter_process(&state, INT32_MAX, 0, UINT32_MAX - 8, &motion);
    nape_scroll_axis_filter_process(&state, INT32_MAX, 0, 8, &motion);
    assert(motion.scroll_x == INT32_MAX);
    nape_scroll_axis_filter_reset(&state);
    nape_scroll_axis_filter_process(&state, INT32_MIN, 0, 100, &motion);
    nape_scroll_axis_filter_process(&state, INT32_MIN, 0, 116, &motion);
    assert(motion.scroll_x == INT32_MIN);
}

int main(void) {
    test_acceleration_profile_and_direction();
    test_acceleration_fraction_and_reversal();
    test_velocity_sampling_and_direction_reversal();
    test_recent_movement_gate_and_expiry();
    test_secondary_axis_and_seed();
    test_positive_negative_decay_and_fractional_stop();
    test_duration_idle_and_epoch_reset();
    test_scroll_start_confirmation_and_idle();
    test_scroll_vertical_release_and_switch();
    test_scroll_confirmation_edges();
    puts("Nape cursor inertia math tests: PASS");
    return 0;
}
