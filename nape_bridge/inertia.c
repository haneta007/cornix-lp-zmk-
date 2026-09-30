/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT nape_input_processor_inertia

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>

#include <drivers/input_processor.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

#include "inertia.h"
#include "inertia_math.h"
#include "scroll_filter.h"
#include "nape_layer_index.h"

#define NAPE_INPUT_NODE(n) DEVICE_DT_GET(DT_NODELABEL(n))

struct nape_inertia_state {
    struct k_spinlock lock;
    int32_t estimate_x_q8;
    int32_t estimate_y_q8;
    struct nape_inertia_axis x;
    struct nape_inertia_axis y;
    struct nape_cursor_accel_axis accel_x;
    struct nape_cursor_accel_axis accel_y;
    struct nape_cursor_motion_history history;
    struct nape_scroll_axis_filter axis_filter;
    uint32_t last_sample_ms;
    uint32_t last_input_ms;
    uint32_t started_ms;
    bool has_previous_sample;
    bool running;
};

static struct nape_inertia_state inertia_state;
static atomic_t inertia_armed;
static atomic_t inertia_epoch;

static void inertia_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(inertia_work, inertia_work_handler);

static zmk_keymap_layer_id_t scroll_layer_id(void) {
    return zmk_keymap_layer_index_to_id(NAPE_SCROLL_LAYER_INDEX);
}

static bool scroll_layer_active(void) {
    return zmk_keymap_layer_active(scroll_layer_id());
}

static void reset_motion_locked(void) {
    inertia_state.estimate_x_q8 = 0;
    inertia_state.estimate_y_q8 = 0;
    nape_inertia_axis_reset(&inertia_state.x);
    nape_inertia_axis_reset(&inertia_state.y);
    nape_cursor_inertia_history_clear(&inertia_state.history);
    inertia_state.last_sample_ms = 0;
    inertia_state.last_input_ms = 0;
    inertia_state.started_ms = 0;
    inertia_state.has_previous_sample = false;
    inertia_state.running = false;
}

static void reset_locked(void) {
    reset_motion_locked();
    nape_cursor_accel_axis_reset(&inertia_state.accel_x);
    nape_cursor_accel_axis_reset(&inertia_state.accel_y);
    nape_scroll_axis_filter_reset(&inertia_state.axis_filter);
}

static void cancel_locked(void) {
    atomic_inc(&inertia_epoch);
    atomic_clear(&inertia_armed);
    reset_motion_locked();
    /* Keep scheduling and cancellation serialized with the work handler. */
    k_work_cancel_delayable(&inertia_work);
}

void nape_inertia_cancel(void) {
    k_spinlock_key_t key = k_spin_lock(&inertia_state.lock);
    cancel_locked();
    k_spin_unlock(&inertia_state.lock, key);
}

void nape_inertia_reset(void) {
    k_spinlock_key_t key = k_spin_lock(&inertia_state.lock);
    cancel_locked();
    reset_locked();
    k_spin_unlock(&inertia_state.lock, key);
}

static void reset_if_epoch(uint32_t expected_epoch) {
    k_spinlock_key_t key = k_spin_lock(&inertia_state.lock);
    if ((uint32_t)atomic_get(&inertia_epoch) == expected_epoch) {
        atomic_inc(&inertia_epoch);
        atomic_clear(&inertia_armed);
        reset_locked();
        k_work_cancel_delayable(&inertia_work);
    }
    k_spin_unlock(&inertia_state.lock, key);
}

void nape_inertia_prepare_motion(int32_t raw_x, int32_t raw_y, uint32_t received_ms,
                                 bool allow_inertia, int32_t *motion_x, int32_t *motion_y) {
    k_spinlock_key_t key = k_spin_lock(&inertia_state.lock);

    if (raw_x == 0 && raw_y == 0) {
        *motion_x = 0;
        *motion_y = 0;
        k_spin_unlock(&inertia_state.lock, key);
        return;
    }

    if (scroll_layer_active()) {
        if (atomic_get(&inertia_armed)) {
            cancel_locked();
        } else {
            reset_motion_locked();
        }
        nape_cursor_accel_axis_reset(&inertia_state.accel_x);
        nape_cursor_accel_axis_reset(&inertia_state.accel_y);

        struct nape_scroll_motion motion;
        nape_scroll_axis_filter_process(&inertia_state.axis_filter, raw_x, raw_y,
                                        received_ms, &motion);
        *motion_x = motion.scroll_x;
        *motion_y = motion.scroll_y;
        k_spin_unlock(&inertia_state.lock, key);
        return;
    }

    nape_scroll_axis_filter_reset(&inertia_state.axis_filter);
    *motion_x = nape_cursor_accel_scale(raw_x, &inertia_state.accel_x);
    *motion_y = nape_cursor_accel_scale(raw_y, &inertia_state.accel_y);

    if (!allow_inertia) {
        cancel_locked();
        k_spin_unlock(&inertia_state.lock, key);
        return;
    }

    if (inertia_state.running) {
        cancel_locked();
    }

    if (inertia_state.has_previous_sample &&
        (uint32_t)(received_ms - inertia_state.last_sample_ms) >
            NAPE_CURSOR_INERTIA_MAX_SAMPLE_GAP_MS) {
        cancel_locked();
    }

    const bool has_previous_sample = inertia_state.has_previous_sample;
    const uint32_t elapsed_ms = has_previous_sample
                                    ? received_ms - inertia_state.last_sample_ms
                                    : 0;
    const int32_t sample_x_q8 = nape_cursor_inertia_sample_velocity(
        *motion_x, elapsed_ms, has_previous_sample);
    const int32_t sample_y_q8 = nape_cursor_inertia_sample_velocity(
        *motion_y, elapsed_ms, has_previous_sample);

    inertia_state.estimate_x_q8 = nape_cursor_inertia_update_velocity(
        inertia_state.estimate_x_q8, sample_x_q8);
    inertia_state.estimate_y_q8 = nape_cursor_inertia_update_velocity(
        inertia_state.estimate_y_q8, sample_y_q8);
    inertia_state.last_sample_ms = received_ms;
    inertia_state.last_input_ms = received_ms;
    inertia_state.has_previous_sample = true;
    inertia_state.running = false;
    inertia_state.started_ms = 0;
    nape_inertia_axis_reset(&inertia_state.x);
    nape_inertia_axis_reset(&inertia_state.y);
    nape_cursor_inertia_history_add(&inertia_state.history, raw_x, raw_y, received_ms);

    atomic_inc(&inertia_epoch);
    atomic_set(&inertia_armed, 1);
    k_work_reschedule(&inertia_work, K_MSEC(NAPE_CURSOR_INERTIA_START_DELAY_MS));
    k_spin_unlock(&inertia_state.lock, key);
}

static int inertia_processor_handle_event(const struct device *dev, struct input_event *event,
                                          uint32_t param1, uint32_t param2,
                                          struct zmk_input_processor_state *processor_state) {
    ARG_UNUSED(dev);
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(processor_state);

    if (event->dev == NAPE_INPUT_NODE(nape_inertia_cursor)) {
        if (event->type != INPUT_EV_REL ||
            (event->code != INPUT_REL_X && event->code != INPUT_REL_Y) ||
            scroll_layer_active() || !atomic_get(&inertia_armed) ||
            !nape_cursor_inertia_event_is_current(
                event->value, (uint32_t)atomic_get(&inertia_epoch))) {
            return ZMK_INPUT_PROC_STOP;
        }
        event->value = nape_cursor_inertia_event_delta(event->value);
        return ZMK_INPUT_PROC_CONTINUE;
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api inertia_processor_api = {
    .handle_event = inertia_processor_handle_event,
};

#define INERTIA_PROCESSOR_INST(n)                                                                  \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                  \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &inertia_processor_api);

DT_INST_FOREACH_STATUS_OKAY(INERTIA_PROCESSOR_INST)

static bool emit_cursor(uint32_t epoch, int32_t x, int32_t y) {
    const struct device *dev = NAPE_INPUT_NODE(nape_inertia_cursor);

    if (x != 0 && y != 0) {
        if (input_report_rel(dev, INPUT_REL_X, nape_cursor_inertia_pack_event(epoch, x),
                             false, K_NO_WAIT) < 0) {
            return false;
        }
        return input_report_rel(dev, INPUT_REL_Y,
                                nape_cursor_inertia_pack_event(epoch, y), true,
                                K_NO_WAIT) >= 0;
    }
    if (x != 0) {
        return input_report_rel(dev, INPUT_REL_X, nape_cursor_inertia_pack_event(epoch, x),
                                true, K_NO_WAIT) >= 0;
    }
    if (y != 0) {
        return input_report_rel(dev, INPUT_REL_Y, nape_cursor_inertia_pack_event(epoch, y),
                                true, K_NO_WAIT) >= 0;
    }
    return true;
}

static void inertia_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    const uint32_t epoch = (uint32_t)atomic_get(&inertia_epoch);
    if (scroll_layer_active()) {
        reset_if_epoch(epoch);
        return;
    }
    if (!atomic_get(&inertia_armed)) {
        return;
    }

    const uint32_t now_ms = k_uptime_get_32();
    int32_t motion_x = 0;
    int32_t motion_y = 0;
    bool continue_work = false;
    uint32_t next_delay_ms = NAPE_CURSOR_INERTIA_TICK_MS;
    k_spinlock_key_t key = k_spin_lock(&inertia_state.lock);

    if (epoch != (uint32_t)atomic_get(&inertia_epoch) ||
        !atomic_get(&inertia_armed)) {
        k_spin_unlock(&inertia_state.lock, key);
        return;
    }

    nape_cursor_inertia_history_prune(&inertia_state.history,
                                      inertia_state.last_input_ms);
    if (!inertia_state.running) {
        const uint32_t idle_ms = now_ms - inertia_state.last_input_ms;
        if (idle_ms < NAPE_CURSOR_INERTIA_START_DELAY_MS) {
            next_delay_ms = NAPE_CURSOR_INERTIA_START_DELAY_MS - idle_ms;
            continue_work = true;
        } else if (nape_cursor_inertia_should_start(
                       &inertia_state.history, inertia_state.estimate_x_q8,
                       inertia_state.estimate_y_q8)) {
            const uint64_t x_magnitude =
                nape_cursor_magnitude(inertia_state.estimate_x_q8);
            const uint64_t y_magnitude =
                nape_cursor_magnitude(inertia_state.estimate_y_q8);
            const uint64_t primary_magnitude =
                x_magnitude >= y_magnitude ? x_magnitude : y_magnitude;

            inertia_state.x.velocity_q8 =
                nape_cursor_inertia_keep_axis(inertia_state.estimate_x_q8,
                                              primary_magnitude)
                    ? nape_cursor_inertia_seed_velocity(inertia_state.estimate_x_q8)
                    : 0;
            inertia_state.y.velocity_q8 =
                nape_cursor_inertia_keep_axis(inertia_state.estimate_y_q8,
                                              primary_magnitude)
                    ? nape_cursor_inertia_seed_velocity(inertia_state.estimate_y_q8)
                    : 0;
            inertia_state.x.remainder_q8 = 0;
            inertia_state.y.remainder_q8 = 0;
            inertia_state.running = inertia_state.x.velocity_q8 != 0 ||
                                    inertia_state.y.velocity_q8 != 0;
            inertia_state.started_ms = now_ms;
        } else {
            atomic_clear(&inertia_armed);
            reset_motion_locked();
        }
    }

    if (inertia_state.running) {
        const uint32_t run_ms = now_ms - inertia_state.started_ms;
        const uint32_t idle_ms = now_ms - inertia_state.last_input_ms;
        if (nape_cursor_inertia_duration_expired(run_ms, idle_ms)) {
            atomic_inc(&inertia_epoch);
            atomic_clear(&inertia_armed);
            reset_motion_locked();
        } else {
            motion_x = nape_cursor_inertia_axis_step(&inertia_state.x);
            motion_y = nape_cursor_inertia_axis_step(&inertia_state.y);
            if (inertia_state.x.velocity_q8 != 0 ||
                inertia_state.y.velocity_q8 != 0) {
                continue_work = true;
            } else {
                inertia_state.running = false;
                inertia_state.estimate_x_q8 = 0;
                inertia_state.estimate_y_q8 = 0;
            }
        }
    }
    k_spin_unlock(&inertia_state.lock, key);

    /* A newer BLE report invalidates queued output through the epoch check. */
    if (epoch != (uint32_t)atomic_get(&inertia_epoch)) {
        return;
    }
    if (scroll_layer_active()) {
        reset_if_epoch(epoch);
        return;
    }
    if (!atomic_get(&inertia_armed)) {
        return;
    }

    if (!emit_cursor(epoch, motion_x, motion_y)) {
        key = k_spin_lock(&inertia_state.lock);
        if (continue_work && !scroll_layer_active() &&
            atomic_get(&inertia_armed) &&
            epoch == (uint32_t)atomic_get(&inertia_epoch)) {
            k_work_reschedule(&inertia_work, K_MSEC(next_delay_ms));
        }
        k_spin_unlock(&inertia_state.lock, key);
        return;
    }

    key = k_spin_lock(&inertia_state.lock);
    if (continue_work && !scroll_layer_active() &&
        atomic_get(&inertia_armed) &&
        epoch == (uint32_t)atomic_get(&inertia_epoch)) {
        k_work_reschedule(&inertia_work, K_MSEC(next_delay_ms));
    }
    k_spin_unlock(&inertia_state.lock, key);
}

static int inertia_layer_listener(const zmk_event_t *event) {
    const struct zmk_layer_state_changed *state = as_zmk_layer_state_changed(event);
    if (state && state->layer == scroll_layer_id()) {
        nape_inertia_reset();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(nape_inertia, inertia_layer_listener);
ZMK_SUBSCRIPTION(nape_inertia, zmk_layer_state_changed);

static int inertia_position_listener(const zmk_event_t *event) {
    const struct zmk_position_state_changed *state =
        as_zmk_position_state_changed(event);
    if (state && state->state) {
        nape_inertia_cancel();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(nape_cursor_inertia_position, inertia_position_listener);
ZMK_SUBSCRIPTION(nape_cursor_inertia_position, zmk_position_state_changed);
