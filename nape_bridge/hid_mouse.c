/* SPDX-License-Identifier: MIT */
#include "hid_mouse.h"

#include <errno.h>
#include <string.h>

#define HID_PAGE_DESKTOP 0x01
#define HID_PAGE_BUTTON 0x09
#define HID_PAGE_CONSUMER 0x0c
#define HID_USAGE_MOUSE 0x02
#define HID_USAGE_X 0x30
#define HID_USAGE_Y 0x31
#define HID_USAGE_WHEEL 0x38
#define HID_USAGE_HWHEEL 0x48
#define HID_USAGE_AC_PAN 0x0238

struct hid_globals {
    uint16_t usage_page;
    int32_t logical_min;
    uint32_t report_size;
    uint32_t report_count;
    uint8_t report_id;
};

struct hid_locals {
    uint32_t usages[32];
    uint8_t count;
    uint32_t minimum;
    uint32_t maximum;
    bool has_range;
};

static uint32_t item_value(const uint8_t *data, uint8_t size) {
    uint32_t value = 0;
    for (uint8_t i = 0; i < size; i++) {
        value |= (uint32_t)data[i] << (i * 8);
    }
    return value;
}

static int32_t signed_item(uint32_t value, uint8_t size) {
    if (size && size < 4 && (value & (1u << (size * 8 - 1)))) {
        value |= ~((1u << (size * 8)) - 1u);
    }
    return (int32_t)value;
}

static struct nape_hid_report *report_for(struct nape_hid_map *map, uint8_t id) {
    for (uint8_t i = 0; i < map->count; i++) {
        if (map->reports[i].id == id) {
            return &map->reports[i];
        }
    }
    if (map->count == NAPE_HID_MAX_REPORTS) {
        return NULL;
    }
    struct nape_hid_report *report = &map->reports[map->count++];
    report->id = id;
    return report;
}

static uint32_t usage_at(const struct hid_locals *local, uint32_t index) {
    if (index < local->count) {
        return local->usages[index];
    }
    if (local->has_range && local->minimum + index <= local->maximum) {
        return local->minimum + index;
    }
    return 0;
}

static void set_field(struct nape_hid_field *field, uint16_t bit_offset,
                      uint32_t bit_size, bool is_signed) {
    if (!field->present) {
        *field = (struct nape_hid_field){.bit_offset = bit_offset,
                                         .bit_size = (uint8_t)bit_size,
                                         .present = true,
                                         .is_signed = is_signed};
    }
}

int nape_hid_parse_map(const uint8_t *map, size_t length, struct nape_hid_map *out) {
    if (!map || !out || !length) {
        return -EINVAL;
    }
    memset(out, 0, sizeof(*out));
    struct hid_globals global = {0};
    struct hid_globals stack[4];
    uint8_t stack_depth = 0;
    struct hid_locals local = {0};
    bool mouse_scope[8] = {0};
    uint8_t depth = 0;

    for (size_t offset = 0; offset < length;) {
        uint8_t prefix = map[offset++];
        if (prefix == 0xfe) {
            if (length - offset < 2 || length - offset - 2 < map[offset]) {
                return -EINVAL;
            }
            offset += (size_t)map[offset] + 2;
            continue;
        }
        uint8_t size = prefix & 3u;
        if (size == 3) {
            size = 4;
        }
        if (length - offset < size) {
            return -EINVAL;
        }
        uint32_t value = item_value(map + offset, size);
        offset += size;
        uint8_t type = (prefix >> 2) & 3u;
        uint8_t tag = prefix >> 4;

        if (type == 1) {
            switch (tag) {
            case 0: global.usage_page = (uint16_t)value; break;
            case 1: global.logical_min = signed_item(value, size); break;
            case 7: global.report_size = value; break;
            case 8:
                if (!value || value > 255) return -EINVAL;
                global.report_id = (uint8_t)value;
                out->uses_report_ids = true;
                break;
            case 9: global.report_count = value; break;
            case 10:
                if (stack_depth == 4) return -E2BIG;
                stack[stack_depth++] = global;
                break;
            case 11:
                if (!stack_depth) return -EINVAL;
                global = stack[--stack_depth];
                break;
            default: break;
            }
        } else if (type == 2) {
            switch (tag) {
            case 0:
                if (local.count == 32) return -E2BIG;
                local.usages[local.count++] = value;
                break;
            case 1: local.minimum = value; local.has_range = true; break;
            case 2: local.maximum = value; local.has_range = true; break;
            default: break;
            }
        } else if (type == 0) {
            if (tag == 10) {
                if (depth == 8) return -E2BIG;
                bool parent = depth && mouse_scope[depth - 1];
                bool mouse_app = value == 1 && global.usage_page == HID_PAGE_DESKTOP &&
                                 usage_at(&local, 0) == HID_USAGE_MOUSE;
                mouse_scope[depth++] = parent || mouse_app;
            } else if (tag == 12) {
                if (!depth) return -EINVAL;
                depth--;
            } else if (tag == 8) {
                if (!global.report_size || global.report_size > 32 ||
                    global.report_count > NAPE_HID_MAX_REPORT_BITS) return -ENOTSUP;
                struct nape_hid_report *report = report_for(out, global.report_id);
                if (!report) return -ENOSPC;
                uint32_t bits = global.report_size * global.report_count;
                if ((uint32_t)report->bits + bits > NAPE_HID_MAX_REPORT_BITS) return -E2BIG;
                bool variable = (value & 0x02u) != 0;
                bool relative = (value & 0x04u) != 0;
                bool data = (value & 0x01u) == 0;
                if (depth && mouse_scope[depth - 1] && data && variable) {
                    for (uint32_t i = 0; i < global.report_count; i++) {
                        uint32_t usage = usage_at(&local, i);
                        uint16_t bit = report->bits + (uint16_t)(i * global.report_size);
                        if (global.usage_page == HID_PAGE_BUTTON && usage >= 1 &&
                            usage <= NAPE_HID_MAX_BUTTONS && global.report_size == 1) {
                            set_field(&report->buttons[usage - 1], bit, 1, false);
                            report->button_mask |= 1u << (usage - 1);
                        } else if (relative && global.usage_page == HID_PAGE_DESKTOP) {
                            if (usage == HID_USAGE_X) set_field(&report->x, bit, global.report_size, global.logical_min < 0);
                            if (usage == HID_USAGE_Y) set_field(&report->y, bit, global.report_size, global.logical_min < 0);
                            if (usage == HID_USAGE_WHEEL) set_field(&report->wheel, bit, global.report_size, global.logical_min < 0);
                            if (usage == HID_USAGE_HWHEEL) set_field(&report->hwheel, bit, global.report_size, global.logical_min < 0);
                        } else if (relative && global.usage_page == HID_PAGE_CONSUMER && usage == HID_USAGE_AC_PAN) {
                            set_field(&report->hwheel, bit, global.report_size, global.logical_min < 0);
                        }
                    }
                }
                report->bits += (uint16_t)bits;
            }
            memset(&local, 0, sizeof(local));
        }
    }
    if (depth || stack_depth) return -EINVAL;
    for (uint8_t i = 0; i < out->count; i++) {
        const struct nape_hid_report *report = &out->reports[i];
        if (report->x.present || report->y.present || report->wheel.present ||
            report->hwheel.present || report->button_mask) return 0;
    }
    return -ENOTSUP;
}

static int read_field(const struct nape_hid_field *field, const uint8_t *payload,
                      size_t length, int32_t *out) {
    if (!field->present) return 0;
    if (!field->bit_size || field->bit_size > 32 ||
        (size_t)field->bit_offset + field->bit_size > length * 8) return -EMSGSIZE;
    uint32_t value = 0;
    for (uint8_t i = 0; i < field->bit_size; i++) {
        uint16_t bit = field->bit_offset + i;
        value |= ((uint32_t)((payload[bit / 8] >> (bit % 8)) & 1u)) << i;
    }
    if (field->is_signed && field->bit_size < 32 && (value & (1u << (field->bit_size - 1)))) {
        value |= ~((1u << field->bit_size) - 1u);
    }
    *out = (int32_t)value;
    return 0;
}

int nape_hid_parse_input(const struct nape_hid_map *map, uint8_t report_id,
                         const uint8_t *payload, size_t length, struct nape_mouse_input *out) {
    if (!map || !payload || !out) return -EINVAL;
    const struct nape_hid_report *report = NULL;
    for (uint8_t i = 0; i < map->count; i++) {
        if (map->reports[i].id == report_id) {
            report = &map->reports[i];
            break;
        }
    }
    if (!report) return -ENOENT;
    if (length != (report->bits + 7u) / 8u) return -EMSGSIZE;
    memset(out, 0, sizeof(*out));
    out->has_x = report->x.present;
    out->has_y = report->y.present;
    out->has_wheel = report->wheel.present;
    out->has_hwheel = report->hwheel.present;
    out->button_mask = report->button_mask;
    if (read_field(&report->x, payload, length, &out->x) ||
        read_field(&report->y, payload, length, &out->y) ||
        read_field(&report->wheel, payload, length, &out->wheel) ||
        read_field(&report->hwheel, payload, length, &out->hwheel)) return -EMSGSIZE;
    for (uint8_t i = 0; i < NAPE_HID_MAX_BUTTONS; i++) {
        int32_t pressed = 0;
        if (read_field(&report->buttons[i], payload, length, &pressed)) return -EMSGSIZE;
        if (pressed) out->buttons |= 1u << i;
    }
    return 0;
}
