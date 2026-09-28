/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../inertia_math.h"

static void test_velocity_sampling_and_threshold(void) {
    int32_t slow = nape_inertia_sample_velocity(3, 16, true);
    int32_t fast = nape_inertia_sample_velocity(16, 16, true);
    assert(!nape_inertia_meets_start_threshold(slow));
    assert(nape_inertia_meets_start_threshold(fast));
    assert(nape_inertia_meets_start_threshold(-fast));
    assert(nape_inertia_sample_velocity(127, 16, true) ==
           NAPE_SCROLL_INERTIA_MAX_COUNTS_PER_TICK * NAPE_SCROLL_INERTIA_VELOCITY_SCALE);
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
    test_velocity_smoothing_and_reversal();
    test_positive_and_negative_decay();
    test_fractional_accumulation_and_stop();
    test_max_duration_and_reset();
    test_queued_scroll_epoch_and_signed_delta();
    puts("Nape inertia math tests: PASS");
    return 0;
}
