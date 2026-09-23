/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT nape_virtual_pointer

#include <errno.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zmk/split/bluetooth/central_scan.h>

#include "hid_mouse.h"

LOG_MODULE_REGISTER(nape, CONFIG_ZMK_NAPE_DEBUG ? LOG_LEVEL_DBG : LOG_LEVEL_INF);

#define NAPE_MAX_GATT_REPORTS 8
#define NAPE_REPORT_MAP_SIZE 512
#define NAPE_MAX_NOTIFICATION 64
#define NAPE_INPUT_QUEUE_SIZE 16

static int virtual_pointer_init(const struct device *dev) { return 0; }
#define NAPE_POINTER_DEVICE(inst)                                                                  \
    DEVICE_DT_INST_DEFINE(inst, virtual_pointer_init, NULL, NULL, NULL, POST_KERNEL,              \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, NULL);
DT_INST_FOREACH_STATUS_OKAY(NAPE_POINTER_DEVICE)

struct gatt_report {
    uint16_t declaration;
    uint16_t value;
    uint16_t descriptor_end;
    uint16_t ccc;
    uint16_t reference;
    uint8_t id;
    uint8_t type;
    struct bt_gatt_subscribe_params subscription;
};

struct bridge_state {
    struct bt_conn *conn;
    bt_addr_le_t candidate;
    atomic_t candidate_ready;
    atomic_t scanning;
    atomic_t connecting;
    uint8_t backoff_step;
    uint16_t hid_start;
    uint16_t hid_end;
    uint16_t map_handle;
    uint16_t protocol_handle;
    uint16_t boot_mouse_handle;
    struct gatt_report reports[NAPE_MAX_GATT_REPORTS];
    uint8_t report_count;
    uint8_t report_index;
    struct bt_gatt_discover_params discovery;
    struct bt_gatt_read_params read;
    uint8_t map[NAPE_REPORT_MAP_SIZE];
    size_t map_length;
    struct nape_hid_map parsed_map;
    uint8_t held_buttons;
    uint32_t generation;
};

static struct bridge_state bridge;
static atomic_t gatt_pending;
K_MUTEX_DEFINE(nape_scan_lock);
K_MUTEX_DEFINE(nape_state_lock);

struct queued_input {
    uint32_t generation;
    uint8_t report_id;
    uint8_t length;
    uint8_t payload[NAPE_MAX_NOTIFICATION];
};
K_MSGQ_DEFINE(nape_input_queue, sizeof(struct queued_input), NAPE_INPUT_QUEUE_SIZE, 4);

static void scan_work_handler(struct k_work *work);
static void scan_timeout_handler(struct k_work *work);
static void candidate_work_handler(struct k_work *work);
static void discovery_work_handler(struct k_work *work);
static void input_work_handler(struct k_work *work);
static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                         struct net_buf_simple *ad);
K_WORK_DELAYABLE_DEFINE(nape_scan_work, scan_work_handler);
K_WORK_DELAYABLE_DEFINE(nape_scan_timeout_work, scan_timeout_handler);
K_WORK_DEFINE(nape_candidate_work, candidate_work_handler);
K_WORK_DEFINE(nape_discovery_work, discovery_work_handler);
K_WORK_DEFINE(nape_input_work, input_work_handler);

static int stop_own_scan(void) {
    /* Called by the split central from Bluetooth callbacks. Never wait for a
     * scanner transition that may itself be waiting for an HCI response. */
    int err = k_mutex_lock(&nape_scan_lock, K_NO_WAIT);
    if (err) return -EAGAIN;
    if (atomic_get(&bridge.scanning)) {
        err = bt_le_scan_stop();
        if (!err || err == -EALREADY) {
            atomic_clear(&bridge.scanning);
            err = 0;
        }
    }
    k_mutex_unlock(&nape_scan_lock);
    if (!err) {
        k_work_cancel_delayable(&nape_scan_timeout_work);
        k_work_reschedule(&nape_scan_work, K_SECONDS(1));
    }
    return err;
}

static void schedule_scan_backoff(void) {
    uint32_t delay = 1000u << MIN(bridge.backoff_step, 5);
    if (bridge.backoff_step < 5) bridge.backoff_step++;
    LOG_INF("NAPE: reconnect scheduled in %u ms", delay);
    k_work_reschedule(&nape_scan_work, K_MSEC(delay));
}

static bool has_connection(void) {
    k_mutex_lock(&nape_state_lock, K_FOREVER);
    bool connected = bridge.conn != NULL;
    k_mutex_unlock(&nape_state_lock);
    return connected;
}

static void scan_work_handler(struct k_work *work) {
    k_mutex_lock(&nape_scan_lock, K_FOREVER);
    if (has_connection() || atomic_get(&bridge.connecting) || atomic_get(&bridge.scanning)) {
        k_mutex_unlock(&nape_scan_lock);
        return;
    }
    if (!zmk_split_ble_peripherals_ready()) {
        k_mutex_unlock(&nape_scan_lock);
        k_work_reschedule(&nape_scan_work, K_SECONDS(1));
        return;
    }
    int err = bt_le_scan_start(BT_LE_SCAN_ACTIVE, device_found);
    if (!err) {
        atomic_set(&bridge.scanning, 1);
        atomic_clear(&bridge.candidate_ready);
        LOG_INF("NAPE: scan start");
        k_work_reschedule(&nape_scan_timeout_work, K_SECONDS(10));
    }
    k_mutex_unlock(&nape_scan_lock);
    if (err) schedule_scan_backoff();
}

static void scan_timeout_handler(struct k_work *work) {
    k_mutex_lock(&nape_scan_lock, K_FOREVER);
    if (!atomic_get(&bridge.scanning)) {
        k_mutex_unlock(&nape_scan_lock);
        return;
    }
    int err = bt_le_scan_stop();
    if (!err || err == -EALREADY) {
        atomic_clear(&bridge.scanning);
        atomic_clear(&bridge.candidate_ready);
        err = 0;
    }
    k_mutex_unlock(&nape_scan_lock);
    if (err) {
        LOG_WRN("NAPE: scan timeout stop failed (%d)", err);
        k_work_reschedule(&nape_scan_timeout_work, K_SECONDS(2));
    } else {
        schedule_scan_backoff();
    }
}

static bool parse_name(struct bt_data *data, void *user_data) {
    bool *matched = user_data;
    if (data->type != BT_DATA_NAME_COMPLETE && data->type != BT_DATA_NAME_SHORTENED) return true;
    size_t name_length = strlen(CONFIG_ZMK_NAPE_NAME);
    if (!name_length || data->data_len < name_length) return true;
    for (size_t i = 0; i <= data->data_len - name_length; i++) {
        if (!memcmp(data->data + i, CONFIG_ZMK_NAPE_NAME, name_length)) {
            *matched = true;
            return false;
        }
    }
    return true;
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                         struct net_buf_simple *ad) {
    if (!atomic_get(&bridge.scanning) || atomic_get(&bridge.candidate_ready) ||
        has_connection() || atomic_get(&bridge.connecting) ||
        (type != BT_GAP_ADV_TYPE_ADV_IND && type != BT_GAP_ADV_TYPE_ADV_SCAN_IND &&
         type != BT_GAP_ADV_TYPE_SCAN_RSP)) return;
    bool matched = false;
    bt_data_parse(ad, parse_name, &matched);
    if (!matched) return;
    bt_addr_le_copy(&bridge.candidate, addr);
    atomic_set(&bridge.candidate_ready, 1);
    LOG_INF("NAPE: candidate found (RSSI %d)", rssi);
    k_work_submit(&nape_candidate_work);
}

static void candidate_work_handler(struct k_work *work) {
    k_mutex_lock(&nape_scan_lock, K_FOREVER);
    if (!atomic_get(&bridge.candidate_ready) || !atomic_get(&bridge.scanning) ||
        !zmk_split_ble_peripherals_ready()) {
        atomic_clear(&bridge.candidate_ready);
        k_mutex_unlock(&nape_scan_lock);
        return;
    }
    int err = bt_le_scan_stop();
    if (err) {
        k_mutex_unlock(&nape_scan_lock);
        schedule_scan_backoff();
        return;
    }
    atomic_clear(&bridge.scanning);
    atomic_clear(&bridge.candidate_ready);
    atomic_set(&bridge.connecting, 1);
    k_work_cancel_delayable(&nape_scan_timeout_work);
    k_mutex_lock(&nape_state_lock, K_FOREVER);
    err = bt_conn_le_create(&bridge.candidate, BT_CONN_LE_CREATE_CONN,
                            BT_LE_CONN_PARAM_DEFAULT, &bridge.conn);
    if (err) {
        atomic_clear(&bridge.connecting);
        bridge.conn = NULL;
    }
    k_mutex_unlock(&nape_state_lock);
    k_mutex_unlock(&nape_scan_lock);
    if (err) schedule_scan_backoff();
}

static bool active_conn(struct bt_conn *conn) {
    k_mutex_lock(&nape_state_lock, K_FOREVER);
    bool active = conn && conn == bridge.conn;
    k_mutex_unlock(&nape_state_lock);
    return active;
}

static uint8_t report_notify(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                             const void *data, uint16_t length) {
    if (!data) return BT_GATT_ITER_STOP;
    if (!active_conn(conn)) return BT_GATT_ITER_CONTINUE;
    struct gatt_report *report = CONTAINER_OF(params, struct gatt_report, subscription);
    if (length > NAPE_MAX_NOTIFICATION) {
        LOG_WRN("NAPE: oversized input report %u", length);
        return BT_GATT_ITER_CONTINUE;
    }
    struct queued_input queued = {.generation = bridge.generation, .report_id = report->id,
                                  .length = (uint8_t)length};
    memcpy(queued.payload, data, length);
    if (k_msgq_put(&nape_input_queue, &queued, K_NO_WAIT)) {
        LOG_WRN("NAPE: input queue full");
    } else {
        k_work_submit(&nape_input_work);
    }
    return BT_GATT_ITER_CONTINUE;
}

static void subscription_done(struct bt_conn *conn, uint8_t err,
                              struct bt_gatt_subscribe_params *params) {
    if (!active_conn(conn)) return;
    struct gatt_report *report = CONTAINER_OF(params, struct gatt_report, subscription);
    if (err) LOG_ERR("NAPE: subscribe id=%u failed (%u)", report->id, err);
    else {
        bridge.backoff_step = 0;
        LOG_INF("NAPE: subscribed report id=%u", report->id);
    }
}

static void subscribe_reports(void) {
    unsigned count = 0;
    for (uint8_t i = 0; i < bridge.report_count; i++) {
        struct gatt_report *report = &bridge.reports[i];
        if (report->type != 1 || !report->ccc || !report->reference) continue;
        bool mouse = false;
        for (uint8_t m = 0; m < bridge.parsed_map.count; m++) {
            const struct nape_hid_report *parsed = &bridge.parsed_map.reports[m];
            if (parsed->id == report->id &&
                (parsed->x.present || parsed->y.present || parsed->wheel.present ||
                 parsed->hwheel.present || parsed->button_mask)) mouse = true;
        }
        if (!mouse && !IS_ENABLED(CONFIG_ZMK_NAPE_DEBUG)) continue;
        report->subscription.value_handle = report->value;
        report->subscription.ccc_handle = report->ccc;
        report->subscription.value = BT_GATT_CCC_NOTIFY;
        report->subscription.notify = report_notify;
        report->subscription.subscribe = subscription_done;
        atomic_set_bit(report->subscription.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);
        int err = bt_gatt_subscribe(bridge.conn, &report->subscription);
        if (err) LOG_ERR("NAPE: subscribe id=%u failed (%d)", report->id, err);
        else count++;
    }
    if (!count) LOG_ERR("NAPE: no supported mouse input report subscription");
}

static uint8_t read_map_cb(struct bt_conn *conn, uint8_t err,
                           struct bt_gatt_read_params *params, const void *data,
                           uint16_t length) {
    if (!active_conn(conn)) return BT_GATT_ITER_STOP;
    if (err) {
        LOG_ERR("NAPE: report map read failed (%u)", err);
        atomic_clear(&gatt_pending);
        return BT_GATT_ITER_STOP;
    }
    if (data) {
        if (bridge.map_length + length > sizeof(bridge.map)) {
            LOG_ERR("NAPE: report map exceeds %u bytes", (unsigned)sizeof(bridge.map));
            atomic_clear(&gatt_pending);
            return BT_GATT_ITER_STOP;
        }
        memcpy(bridge.map + bridge.map_length, data, length);
        bridge.map_length += length;
        return BT_GATT_ITER_CONTINUE;
    }
    atomic_clear(&gatt_pending);
    LOG_INF("NAPE: report map read (%u bytes)", (unsigned)bridge.map_length);
    LOG_HEXDUMP_DBG(bridge.map, bridge.map_length, "NAPE: report map raw");
    int rc = nape_hid_parse_map(bridge.map, bridge.map_length, &bridge.parsed_map);
    if (rc) LOG_ERR("NAPE: unsupported report map (%d)", rc);
    if (!rc || IS_ENABLED(CONFIG_ZMK_NAPE_DEBUG)) subscribe_reports();
    return BT_GATT_ITER_STOP;
}

static void read_map(struct bt_conn *conn, uint16_t map_handle) {
    if (!map_handle) {
        atomic_clear(&gatt_pending);
        LOG_ERR("NAPE: HID report map missing");
        return;
    }
    bridge.map_length = 0;
    memset(&bridge.read, 0, sizeof(bridge.read));
    bridge.read.func = read_map_cb;
    bridge.read.handle_count = 1;
    bridge.read.single.handle = map_handle;
    int err = bt_gatt_read(conn, &bridge.read);
    if (err) {
        atomic_clear(&gatt_pending);
        LOG_ERR("NAPE: report map read start failed (%d)", err);
    }
}

static uint8_t read_reference_cb(struct bt_conn *conn, uint8_t err,
                                 struct bt_gatt_read_params *params, const void *data,
                                 uint16_t length) {
    if (!active_conn(conn)) return BT_GATT_ITER_STOP;
    if (err) {
        atomic_clear(&gatt_pending);
        LOG_ERR("NAPE: Report Reference read failed (%u)", err);
        goto next_report;
    }
    if (data) {
        atomic_clear(&gatt_pending);
        if (length == 2) {
            bridge.reports[bridge.report_index].id = ((const uint8_t *)data)[0];
            bridge.reports[bridge.report_index].type = ((const uint8_t *)data)[1];
        } else {
            LOG_WRN("NAPE: invalid Report Reference length %u", length);
        }
        goto next_report;
    }
    atomic_clear(&gatt_pending);
    goto next_report;
next_report:
    bridge.report_index++;
    k_work_submit(&nape_discovery_work);
    return BT_GATT_ITER_STOP;
}

static uint8_t descriptor_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             struct bt_gatt_discover_params *params) {
    if (!active_conn(conn)) return BT_GATT_ITER_STOP;
    if (attr) {
        struct gatt_report *report = &bridge.reports[bridge.report_index];
        if (!bt_uuid_cmp(attr->uuid, BT_UUID_GATT_CCC)) report->ccc = attr->handle;
        if (!bt_uuid_cmp(attr->uuid, BT_UUID_HIDS_REPORT_REF)) report->reference = attr->handle;
        return BT_GATT_ITER_CONTINUE;
    }
    struct gatt_report *report = &bridge.reports[bridge.report_index];
    if (report->reference) {
        memset(&bridge.read, 0, sizeof(bridge.read));
        bridge.read.func = read_reference_cb;
        bridge.read.handle_count = 1;
        bridge.read.single.handle = report->reference;
        int err = bt_gatt_read(conn, &bridge.read);
        if (!err) return BT_GATT_ITER_STOP;
        LOG_ERR("NAPE: Report Reference read start failed (%d)", err);
    }
    atomic_clear(&gatt_pending);
    bridge.report_index++;
    k_work_submit(&nape_discovery_work);
    return BT_GATT_ITER_STOP;
}

static uint8_t characteristic_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 struct bt_gatt_discover_params *params) {
    if (!active_conn(conn)) return BT_GATT_ITER_STOP;
    if (attr) {
        const struct bt_gatt_chrc *chrc = attr->user_data;
        if (!chrc) return BT_GATT_ITER_CONTINUE;
        if (bridge.report_count &&
            !bridge.reports[bridge.report_count - 1].descriptor_end) {
            bridge.reports[bridge.report_count - 1].descriptor_end = attr->handle - 1;
        }
        if (!bt_uuid_cmp(chrc->uuid, BT_UUID_HIDS_REPORT_MAP)) bridge.map_handle = chrc->value_handle;
        else if (!bt_uuid_cmp(chrc->uuid, BT_UUID_HIDS_PROTOCOL_MODE)) bridge.protocol_handle = chrc->value_handle;
        else if (!bt_uuid_cmp(chrc->uuid, BT_UUID_HIDS_BOOT_MOUSE_IN_REPORT)) bridge.boot_mouse_handle = chrc->value_handle;
        else if (!bt_uuid_cmp(chrc->uuid, BT_UUID_HIDS_REPORT)) {
            if (bridge.report_count == NAPE_MAX_GATT_REPORTS) {
                LOG_ERR("NAPE: too many HID reports");
                return BT_GATT_ITER_STOP;
            }
            struct gatt_report *report = &bridge.reports[bridge.report_count++];
            report->declaration = attr->handle;
            report->value = chrc->value_handle;
        }
        return BT_GATT_ITER_CONTINUE;
    }
    atomic_clear(&gatt_pending);
    if (bridge.protocol_handle) {
        static const uint8_t report_mode = 1;
        int err = bt_gatt_write_without_response(conn, bridge.protocol_handle, &report_mode, 1, false);
        if (err) LOG_WRN("NAPE: protocol mode write failed (%d)", err);
    }
    bridge.report_index = 0;
    k_work_submit(&nape_discovery_work);
    return BT_GATT_ITER_STOP;
}

static uint8_t service_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          struct bt_gatt_discover_params *params) {
    if (!active_conn(conn)) return BT_GATT_ITER_STOP;
    if (!attr || !attr->user_data) {
        atomic_clear(&gatt_pending);
        LOG_ERR("NAPE: HID service missing");
        return BT_GATT_ITER_STOP;
    }
    const struct bt_gatt_service_val *service = attr->user_data;
    bridge.hid_start = attr->handle + 1;
    bridge.hid_end = service->end_handle;
    atomic_clear(&gatt_pending);
    LOG_INF("NAPE: HID service found (%u-%u)", bridge.hid_start, bridge.hid_end);
    k_work_submit(&nape_discovery_work);
    return BT_GATT_ITER_STOP;
}

static void discovery_work_handler(struct k_work *work) {
    k_mutex_lock(&nape_state_lock, K_FOREVER);
    if (!bridge.conn || bt_conn_get_security(bridge.conn) < BT_SECURITY_L2 ||
        !atomic_cas(&gatt_pending, 0, 1)) {
        k_mutex_unlock(&nape_state_lock);
        return;
    }
    struct bt_conn *conn = bt_conn_ref(bridge.conn);
    memset(&bridge.discovery, 0, sizeof(bridge.discovery));
    if (!bridge.hid_start) {
        memset(bridge.reports, 0, sizeof(bridge.reports));
        memset(&bridge.parsed_map, 0, sizeof(bridge.parsed_map));
        bridge.discovery.uuid = BT_UUID_HIDS;
        bridge.discovery.func = service_cb;
        bridge.discovery.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
        bridge.discovery.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
        bridge.discovery.type = BT_GATT_DISCOVER_PRIMARY;
    } else if (!bridge.map_handle && !bridge.report_count) {
        bridge.discovery.func = characteristic_cb;
        bridge.discovery.start_handle = bridge.hid_start;
        bridge.discovery.end_handle = bridge.hid_end;
        bridge.discovery.type = BT_GATT_DISCOVER_CHARACTERISTIC;
    } else if (bridge.report_index < bridge.report_count) {
        struct gatt_report *report = &bridge.reports[bridge.report_index];
        uint16_t end = report->descriptor_end ? report->descriptor_end : bridge.hid_end;
        if (report->value >= end) {
            bridge.report_index++;
            atomic_clear(&gatt_pending);
            k_mutex_unlock(&nape_state_lock);
            bt_conn_unref(conn);
            k_work_submit(&nape_discovery_work);
            return;
        }
        bridge.discovery.func = descriptor_cb;
        bridge.discovery.start_handle = report->value + 1;
        bridge.discovery.end_handle = end;
        bridge.discovery.type = BT_GATT_DISCOVER_DESCRIPTOR;
    } else {
        uint16_t map_handle = bridge.map_handle;
        k_mutex_unlock(&nape_state_lock);
        read_map(conn, map_handle);
        bt_conn_unref(conn);
        return;
    }
    k_mutex_unlock(&nape_state_lock);
    int err = bt_gatt_discover(conn, &bridge.discovery);
    if (err) {
        atomic_clear(&gatt_pending);
        LOG_ERR("NAPE: discovery start failed (%d)", err);
    }
    bt_conn_unref(conn);
}

static void connected(struct bt_conn *conn, uint8_t err) {
    if (!active_conn(conn)) return;
    atomic_clear(&bridge.connecting);
    if (err) {
        LOG_ERR("NAPE: connection failed (%u)", err);
        k_mutex_lock(&nape_state_lock, K_FOREVER);
        bt_conn_unref(bridge.conn);
        bridge.conn = NULL;
        k_mutex_unlock(&nape_state_lock);
        schedule_scan_backoff();
        return;
    }
    LOG_INF("NAPE: connected");
    int rc = bt_conn_set_security(conn, BT_SECURITY_L2);
    if (rc && rc != -EALREADY) LOG_ERR("NAPE: security request failed (%d)", rc);
    if (bt_conn_get_security(conn) >= BT_SECURITY_L2) k_work_submit(&nape_discovery_work);
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    if (!active_conn(conn)) return;
    LOG_INF("NAPE: disconnected (%u)", reason);
    k_mutex_lock(&nape_state_lock, K_FOREVER);
    bridge.generation++;
    atomic_clear(&gatt_pending);
    bt_conn_unref(bridge.conn);
    bridge.conn = NULL;
    atomic_clear(&bridge.connecting);
    bridge.hid_start = 0;
    bridge.hid_end = 0;
    bridge.map_handle = 0;
    bridge.protocol_handle = 0;
    bridge.boot_mouse_handle = 0;
    bridge.report_count = 0;
    bridge.report_index = 0;
    bridge.map_length = 0;
    k_mutex_unlock(&nape_state_lock);
    k_work_submit(&nape_input_work);
    schedule_scan_backoff();
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
                             enum bt_security_err err) {
    if (!active_conn(conn)) return;
    if (err) {
        LOG_ERR("NAPE: security failed (%u)", err);
        bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
    } else if (level >= BT_SECURITY_L2) {
        LOG_INF("NAPE: security established (level %u)", level);
        k_work_submit(&nape_discovery_work);
    }
}

BT_CONN_CB_DEFINE(nape_connection_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .security_changed = security_changed,
};

static int emit_relative(const struct device *dev, uint16_t code, int32_t value, bool sync) {
    if (!value) return 0;
    while (value) {
        int32_t chunk = CLAMP(value, INT16_MIN, INT16_MAX);
        int err = input_report_rel(dev, code, chunk, sync && value == chunk, K_NO_WAIT);
        if (err) return err;
        value -= chunk;
    }
    return 0;
}

static void input_work_handler(struct k_work *work) {
    const struct device *motion = DEVICE_DT_GET(DT_NODELABEL(nape_motion));
    const struct device *controls = DEVICE_DT_GET(DT_NODELABEL(nape_controls));
    k_mutex_lock(&nape_state_lock, K_FOREVER);
    if (!bridge.conn && bridge.held_buttons) {
        for (uint8_t i = 0; i < 5; i++) {
            if (bridge.held_buttons & BIT(i)) input_report_key(controls, INPUT_BTN_0 + i, 0, true, K_NO_WAIT);
        }
        bridge.held_buttons = 0;
    }
    struct queued_input queued;
    while (!k_msgq_get(&nape_input_queue, &queued, K_NO_WAIT)) {
        if (!bridge.conn || queued.generation != bridge.generation) continue;
        struct nape_mouse_input parsed;
        int err = nape_hid_parse_input(&bridge.parsed_map, queued.report_id,
                                       queued.payload, queued.length, &parsed);
        if (err) {
            LOG_WRN("NAPE: unsupported input id=%u len=%u (%d)", queued.report_id, queued.length, err);
            LOG_HEXDUMP_DBG(queued.payload, queued.length, "NAPE: input raw");
            continue;
        }
        LOG_DBG("NAPE: input id=%u x=%d y=%d wheel=%d buttons=%02x", queued.report_id,
                parsed.x, parsed.y, parsed.wheel, parsed.buttons);
        if (parsed.x || parsed.y) {
            emit_relative(motion, INPUT_REL_X, parsed.x, !parsed.y);
            emit_relative(motion, INPUT_REL_Y, parsed.y, true);
        }
        if (parsed.wheel) emit_relative(controls, INPUT_REL_WHEEL, parsed.wheel, true);
        if (parsed.hwheel) emit_relative(controls, INPUT_REL_HWHEEL, parsed.hwheel, true);
        uint8_t changed = (bridge.held_buttons ^ parsed.buttons) & parsed.button_mask & 0x1f;
        for (uint8_t i = 0; i < 5; i++) {
            if (changed & BIT(i)) {
                input_report_key(controls, INPUT_BTN_0 + i, !!(parsed.buttons & BIT(i)),
                                 true, K_NO_WAIT);
            }
        }
        bridge.held_buttons = (bridge.held_buttons & ~parsed.button_mask) |
                              (parsed.buttons & parsed.button_mask);
    }
    k_mutex_unlock(&nape_state_lock);
}

static int nape_init(void) {
    int err = zmk_split_ble_register_external_scan_stop(stop_own_scan);
    if (err) return err;
    k_work_reschedule(&nape_scan_work, K_SECONDS(1));
    return 0;
}
SYS_INIT(nape_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
