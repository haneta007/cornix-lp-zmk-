/* SPDX-License-Identifier: MIT */
#include "../hid_mouse.h"

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

int main(void) {
    without_report_id();
    with_report_id();
    malformed();
    puts("Nape HID mouse parser: PASS");
    return 0;
}
