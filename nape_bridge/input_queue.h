/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NAPE_INPUT_QUEUE_CAPACITY 4
#define NAPE_INPUT_MAX_NOTIFICATION 64

struct nape_queued_input {
    uint32_t generation;
    uint32_t received_ms;
    uint8_t report_id;
    uint8_t length;
    uint8_t payload[NAPE_INPUT_MAX_NOTIFICATION];
};

struct nape_input_queue {
    struct nape_queued_input entries[NAPE_INPUT_QUEUE_CAPACITY];
    uint8_t head;
    uint8_t count;
    uint32_t overflow_count;
    bool release_buttons_pending;
};

static inline void nape_input_queue_request_button_release(struct nape_input_queue *queue) {
    queue->release_buttons_pending = true;
}

/* The caller serializes these operations; the BLE bridge uses a spinlock. */
static inline bool nape_input_queue_push(struct nape_input_queue *queue,
                                         const struct nape_queued_input *input) {
    bool overflowed = queue->count == NAPE_INPUT_QUEUE_CAPACITY;
    if (overflowed) {
        queue->head = (queue->head + 1u) % NAPE_INPUT_QUEUE_CAPACITY;
        queue->count--;
        queue->overflow_count++;
        queue->release_buttons_pending = true;
    }

    uint8_t tail = (queue->head + queue->count) % NAPE_INPUT_QUEUE_CAPACITY;
    queue->entries[tail] = *input;
    queue->count++;
    return overflowed;
}

static inline bool nape_input_queue_pop(struct nape_input_queue *queue,
                                        struct nape_queued_input *input,
                                        bool *release_buttons) {
    *release_buttons = queue->release_buttons_pending;
    queue->release_buttons_pending = false;
    if (!queue->count) return false;

    *input = queue->entries[queue->head];
    queue->head = (queue->head + 1u) % NAPE_INPUT_QUEUE_CAPACITY;
    queue->count--;
    return true;
}
