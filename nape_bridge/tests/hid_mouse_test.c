/* SPDX-License-Identifier: MIT */
#include "../hid_mouse.h"
#include "../input_queue.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

static const uint8_t mouse_map[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05,
    0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xc0, 0xc0,
};

static void without_report_id(void) {
    struct nape_hid_map map;
    assert(nape_hid_parse_map(mouse_map, sizeof(mouse_map), &map) == 0);
    assert(!map.uses_report_ids);
    uint8_t report[] = {0x05, 0xfe, 0x03, 0xff};
    struct nape_mouse_input input;
    assert(nape_hid_parse_input(&map, 0, report, sizeof(report), &input) == 0);
    assert(input.x == -2 && input.y == 3 && input.wheel == -1);
    assert(input.buttons == 0x05 && input.button_mask == 0x07);
    assert(nape_hid_parse_input(&map, 0, report, sizeof(report) - 1, &input) == -EMSGSIZE);
    assert(nape_hid_parse_input(&map, 1, report, sizeof(report), &input) == -ENOENT);
}

static void with_report_id(void) {
    uint8_t descriptor[sizeof(mouse_map) + 2];
    for (size_t i = 0; i < 6; i++) descriptor[i] = mouse_map[i];
    descriptor[6] = 0x85;
    descriptor[7] = 0x02;
    for (size_t i = 6; i < sizeof(mouse_map); i++) descriptor[i + 2] = mouse_map[i];
    struct nape_hid_map map;
    assert(nape_hid_parse_map(descriptor, sizeof(descriptor), &map) == 0);
    assert(map.uses_report_ids);
    uint8_t report[] = {0x02, 0xfd, 0x04, 0x01};
    struct nape_mouse_input input;
    assert(nape_hid_parse_input(&map, 2, report, sizeof(report), &input) == 0);
    assert(input.x == -3 && input.y == 4 && input.wheel == 1 && input.buttons == 2);
    assert(nape_hid_parse_input(&map, 0, report, sizeof(report), &input) == -ENOENT);
}

static void malformed(void) {
    struct nape_hid_map map;
    uint8_t truncated[] = {0x05};
    assert(nape_hid_parse_map(truncated, sizeof(truncated), &map) == -EINVAL);
    assert(nape_hid_parse_map(mouse_map, sizeof(mouse_map) - 1, &map) == -EINVAL);
}

static void input_queue_overflow_keeps_newest_and_requests_button_release(void) {
    struct nape_input_queue queue = {0};
    for (uint8_t i = 0; i < NAPE_INPUT_QUEUE_CAPACITY + 1; i++) {
        struct nape_queued_input input = {.generation = 7,
                                          .received_ms = 1000u + i,
                                          .report_id = 2,
                                          .length = 1,
                                          .payload = {i}};
        bool overflowed = nape_input_queue_push(&queue, &input);
        assert(overflowed == (i == NAPE_INPUT_QUEUE_CAPACITY));
    }

    assert(queue.count == NAPE_INPUT_QUEUE_CAPACITY);
    assert(queue.overflow_count == 1);
    struct nape_queued_input input;
    bool release_buttons;
    for (uint8_t i = 1; i <= NAPE_INPUT_QUEUE_CAPACITY; i++) {
        assert(nape_input_queue_pop(&queue, &input, &release_buttons));
        assert(release_buttons == (i == 1));
        assert(input.payload[0] == i);
        assert(input.received_ms == 1000u + i);
    }
    assert(!nape_input_queue_pop(&queue, &input, &release_buttons));
    assert(!release_buttons);
}

static void input_queue_overflow_after_wraparound(void) {
    struct nape_input_queue queue = {0};
    struct nape_queued_input input;
    bool release_buttons;
    for (uint8_t i = 0; i < NAPE_INPUT_QUEUE_CAPACITY; i++) {
        input = (struct nape_queued_input){.length = 1, .payload = {i}};
        assert(!nape_input_queue_push(&queue, &input));
    }
    assert(nape_input_queue_pop(&queue, &input, &release_buttons) && input.payload[0] == 0);
    assert(!release_buttons);
    assert(nape_input_queue_pop(&queue, &input, &release_buttons) && input.payload[0] == 1);
    assert(!release_buttons);

    for (uint8_t i = 4; i <= 6; i++) {
        input = (struct nape_queued_input){.length = 1, .payload = {i}};
        bool overflowed = nape_input_queue_push(&queue, &input);
        assert(overflowed == (i == 6));
    }
    assert(nape_input_queue_pop(&queue, &input, &release_buttons));
    assert(release_buttons);
    assert(input.payload[0] == 3);
}

static void input_queue_disconnect_release_survives_without_pending_reports(void) {
    struct nape_input_queue queue = {0};
    struct nape_queued_input input;
    bool release_buttons;

    nape_input_queue_request_button_release(&queue);
    assert(!nape_input_queue_pop(&queue, &input, &release_buttons));
    assert(release_buttons);
    assert(!nape_input_queue_pop(&queue, &input, &release_buttons));
    assert(!release_buttons);
}

static void input_queue_preserves_notification_timestamps(void) {
    struct nape_input_queue queue = {0};
    struct nape_queued_input sent = {.generation = 12, .received_ms = UINT32_MAX - 3,
                                     .report_id = 4, .length = 2, .payload = {0xaa, 0x55}};
    struct nape_queued_input received;
    bool release_buttons;

    assert(!nape_input_queue_push(&queue, &sent));
    assert(nape_input_queue_pop(&queue, &received, &release_buttons));
    assert(!release_buttons);
    assert(received.generation == sent.generation);
    assert(received.received_ms == sent.received_ms);
    assert(received.payload[0] == sent.payload[0] && received.payload[1] == sent.payload[1]);
}

int main(void) {
    without_report_id();
    with_report_id();
    malformed();
    input_queue_overflow_keeps_newest_and_requests_button_release();
    input_queue_overflow_after_wraparound();
    input_queue_disconnect_release_survives_without_pending_reports();
    input_queue_preserves_notification_timestamps();
    puts("Nape HID mouse parser: PASS");
    return 0;
}
