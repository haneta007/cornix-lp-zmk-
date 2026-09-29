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
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>

#include "inertia.h"
#include "inertia_math.h"
#include "nape_layer_index.h"

#define NAPE_INPUT_NODE(n) DEVICE_DT_GET(DT_NODELABEL(n))

struct nape_inertia_state {
    struct k_spinlock lock;
    int32_t estimate_x_q8;
    int32_t estimate_y_q8;
    struct nape_inertia_axis x;
    struct nape_inertia_axis y;
    uint32_t last_sample_ms;
    int64_t last_input_ms;
    int64_t started_ms;
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

static bool scroll_layer_active(void) { return zmk_keymap_layer_active(scroll_layer_id()); }

static void reset_locked(void) {
    inertia_state.estimate_x_q8 = 0;
    inertia_state.estimate_y_q8 = 0;
    nape_inertia_axis_reset(&inertia_state.x);
    nape_inertia_axis_reset(&inertia_state.y);
    inertia_state.last_sample_ms = 0;
    inertia_state.last_input_ms = 0;
    inertia_state.started_ms = 0;
    inertia_state.has_previous_sample = false;
    inertia_state.running = false;
}

void nape_inertia_reset(void) {
    k_spinlock_key_t key = k_spin_lock(&inertia_state.lock);
    atomic_inc(&inertia_epoch);
    atomic_clear(&inertia_armed);
    reset_locked();
    k_work_cancel_delayable(&inertia_work);
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

void nape_inertia_track(int32_t dx, int32_t dy, uint32_t received_ms) {
    if (!scroll_layer_active()) {
        if (atomic_get(&inertia_armed)) {
            nape_inertia_reset();
        }
        return;
    }

    const int64_t now_ms = k_uptime_get();
    k_spinlock_key_t key = k_spin_lock(&inertia_state.lock);

    if (!scroll_layer_active()) {
        atomic_inc(&inertia_epoch);
        atomic_clear(&inertia_armed);
        reset_locked();
        k_work_cancel_delayable(&inertia_work);
        k_spin_unlock(&inertia_state.lock, key);
        return;
    }

    if (dx != 0 || dy != 0) {
        const uint32_t elapsed_ms = inertia_state.has_previous_sample
                                       ? received_ms - inertia_state.last_sample_ms
                                       : 0;
        const int32_t sample_x_q8 = nape_inertia_sample_velocity(
            dx, elapsed_ms, inertia_state.has_previous_sample);
        const int32_t sample_y_q8 = nape_inertia_sample_velocity(
            dy, elapsed_ms, inertia_state.has_previous_sample);
        inertia_state.estimate_x_q8 =
            nape_inertia_update_velocity(inertia_state.estimate_x_q8, sample_x_q8);
        inertia_state.estimate_y_q8 =
            nape_inertia_update_velocity(inertia_state.estimate_y_q8, sample_y_q8);
        inertia_state.last_sample_ms = received_ms;
        inertia_state.last_input_ms = now_ms;
        inertia_state.has_previous_sample = true;
        inertia_state.running = false;
        inertia_state.started_ms = 0;
        nape_inertia_axis_reset(&inertia_state.x);
        nape_inertia_axis_reset(&inertia_state.y);
        atomic_inc(&inertia_epoch);
        atomic_set(&inertia_armed, 1);
        /* Serialize rescheduling with reset/cancel under the state lock. */
        k_work_reschedule(&inertia_work, K_MSEC(NAPE_SCROLL_INERTIA_START_DELAY_MS));
    }
    k_spin_unlock(&inertia_state.lock, key);
}

static int inertia_processor_handle_event(const struct device *dev, struct input_event *event,
                                          uint32_t param1, uint32_t param2,
                                          struct zmk_input_processor_state *processor_state) {
    ARG_UNUSED(dev);
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(processor_state);

    if (event->dev == NAPE_INPUT_NODE(nape_inertia_scroll)) {
        if (event->type != INPUT_EV_REL ||
            (event->code != INPUT_REL_WHEEL && event->code != INPUT_REL_HWHEEL) ||
            !scroll_layer_active() || !atomic_get(&inertia_armed) ||
            !nape_inertia_scroll_event_is_current(
                event->value, (uint32_t)atomic_get(&inertia_epoch))) {
            return ZMK_INPUT_PROC_STOP;
        }
        event->value = nape_inertia_scroll_event_delta(event->value);
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

static bool emit_scroll(uint32_t epoch, int32_t hwheel, int32_t wheel) {
    const struct device *dev = NAPE_INPUT_NODE(nape_inertia_scroll);
    if (hwheel != 0 && wheel != 0) {
        if (input_report_rel(dev, INPUT_REL_HWHEEL,
                             nape_inertia_pack_scroll_event(epoch, hwheel), true, K_NO_WAIT) < 0) {
            return false;
        }
        if (input_report_rel(dev, INPUT_REL_WHEEL, nape_inertia_pack_scroll_event(epoch, wheel),
                             true, K_NO_WAIT) < 0) {
            return false;
        }
    } else if (hwheel != 0) {
        if (input_report_rel(dev, INPUT_REL_HWHEEL,
                             nape_inertia_pack_scroll_event(epoch, hwheel), true, K_NO_WAIT) < 0) {
            return false;
        }
    } else if (wheel != 0) {
        if (input_report_rel(dev, INPUT_REL_WHEEL, nape_inertia_pack_scroll_event(epoch, wheel),
                             true, K_NO_WAIT) < 0) {
            return false;
        }
    }
    return true;
}

static void inertia_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    const uint32_t epoch = (uint32_t)atomic_get(&inertia_epoch);
    if (!scroll_layer_active()) {
        reset_if_epoch(epoch);
        return;
    }
    if (!atomic_get(&inertia_armed)) {
        return;
    }

    const int64_t now_ms = k_uptime_get();
    int32_t hwheel = 0;
    int32_t wheel = 0;
    bool continue_work = false;
    uint32_t next_delay_ms = NAPE_SCROLL_INERTIA_TICK_MS;
    k_spinlock_key_t key = k_spin_lock(&inertia_state.lock);

    if (!inertia_state.running) {
        const int64_t idle_ms = now_ms - inertia_state.last_input_ms;
        if (idle_ms < NAPE_SCROLL_INERTIA_START_DELAY_MS) {
            next_delay_ms = NAPE_SCROLL_INERTIA_START_DELAY_MS - (uint32_t)idle_ms;
            continue_work = true;
        } else if (nape_inertia_meets_start_threshold(inertia_state.estimate_x_q8) ||
                   nape_inertia_meets_start_threshold(inertia_state.estimate_y_q8)) {
            inertia_state.x.velocity_q8 = inertia_state.estimate_x_q8;
            inertia_state.y.velocity_q8 = inertia_state.estimate_y_q8;
            inertia_state.x.remainder_q8 = 0;
            inertia_state.y.remainder_q8 = 0;
            inertia_state.running = true;
            inertia_state.started_ms = now_ms;
        } else {
            atomic_clear(&inertia_armed);
            inertia_state.estimate_x_q8 = 0;
            inertia_state.estimate_y_q8 = 0;
        }
    }

    if (inertia_state.running) {
        if (nape_inertia_duration_expired((uint32_t)(now_ms - inertia_state.started_ms))) {
            atomic_inc(&inertia_epoch);
            reset_locked();
            atomic_clear(&inertia_armed);
        } else {
            hwheel = nape_inertia_axis_step(&inertia_state.x);
            wheel = nape_inertia_axis_step(&inertia_state.y);
            if (inertia_state.x.velocity_q8 != 0 || inertia_state.y.velocity_q8 != 0) {
                continue_work = true;
            } else {
                inertia_state.running = false;
                inertia_state.estimate_x_q8 = 0;
                inertia_state.estimate_y_q8 = 0;
            }
        }
    }
    k_spin_unlock(&inertia_state.lock, key);

    /* A newer motion event has already scheduled the next idle check. */
    if (epoch != (uint32_t)atomic_get(&inertia_epoch)) {
        return;
    }
    if (!scroll_layer_active()) {
        reset_if_epoch(epoch);
        return;
    }
    if (!atomic_get(&inertia_armed)) {
        return;
    }

    if (!emit_scroll(epoch, hwheel, wheel)) {
        key = k_spin_lock(&inertia_state.lock);
        if (continue_work && scroll_layer_active() && atomic_get(&inertia_armed) &&
            epoch == (uint32_t)atomic_get(&inertia_epoch)) {
            k_work_reschedule(&inertia_work, K_MSEC(next_delay_ms));
        }
        k_spin_unlock(&inertia_state.lock, key);
        return;
    }

    key = k_spin_lock(&inertia_state.lock);
    if (continue_work && scroll_layer_active() && atomic_get(&inertia_armed) &&
        epoch == (uint32_t)atomic_get(&inertia_epoch)) {
        k_work_reschedule(&inertia_work, K_MSEC(next_delay_ms));
    }
    k_spin_unlock(&inertia_state.lock, key);
}

static int inertia_layer_listener(const zmk_event_t *event) {
    const struct zmk_layer_state_changed *state = as_zmk_layer_state_changed(event);
    if (state && state->layer == scroll_layer_id() && !state->state) {
        nape_inertia_reset();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(nape_inertia, inertia_layer_listener);
ZMK_SUBSCRIPTION(nape_inertia, zmk_layer_state_changed);
