/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NAPE_HID_MAX_REPORTS 8
#define NAPE_HID_MAX_BUTTONS 8
#define NAPE_HID_MAX_REPORT_BITS 512

struct nape_hid_field {
    uint16_t bit_offset;
    uint8_t bit_size;
    bool present;
    bool is_signed;
};

struct nape_hid_report {
    uint8_t id;
    uint16_t bits;
    struct nape_hid_field x;
    struct nape_hid_field y;
    struct nape_hid_field wheel;
    struct nape_hid_field hwheel;
    struct nape_hid_field buttons[NAPE_HID_MAX_BUTTONS];
    uint8_t button_mask;
};

struct nape_hid_map {
    struct nape_hid_report reports[NAPE_HID_MAX_REPORTS];
    uint8_t count;
    bool uses_report_ids;
};

struct nape_mouse_input {
    int32_t x;
    int32_t y;
    int32_t wheel;
    int32_t hwheel;
    uint8_t buttons;
    uint8_t button_mask;
    bool has_x;
    bool has_y;
    bool has_wheel;
    bool has_hwheel;
};

int nape_hid_parse_map(const uint8_t *map, size_t length, struct nape_hid_map *out);
int nape_hid_parse_input(const struct nape_hid_map *map, uint8_t report_id,
                         const uint8_t *payload, size_t length, struct nape_mouse_input *out);
