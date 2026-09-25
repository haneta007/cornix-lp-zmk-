/* SPDX-License-Identifier: MIT */
#define DT_DRV_COMPAT nape_virtual_pointer

#include <errno.h>
#include <stdint.h>
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

#if IS_ENABLED(CONFIG_ZMK_NAPE_DEBUG)
LOG_MODULE_REGISTER(nape, LOG_LEVEL_DBG);
#else
LOG_MODULE_REGISTER(nape, LOG_LEVEL_INF);
#endif

#define NAPE_MAX_GATT_REPORTS 8
#define NAPE_REPORT_MAP_SIZE 512
#define NAPE_MAX_NOTIFICATION 64
#define NAPE_INPUT_QUEUE_SIZE 8

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
    atomic_t services_scanned;
    uint8_t service_count;
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
static bool waiting_for_split;
#if IS_ENABLED(CONFIG_ZMK_NAPE_BOND_DIAGNOSTICS)
static atomic_t bond_inventory_logged;
#endif
#if IS_ENABLED(CONFIG_ZMK_NAPE_BOND_CLEANUP_ONLY)
static atomic_t bond_cleanup_started;
#endif
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
    struct bt_conn *pending = NULL;
    k_mutex_lock(&nape_state_lock, K_FOREVER);
    if (!err && atomic_get(&bridge.connecting) && bridge.conn) {
        pending = bt_conn_ref(bridge.conn);
    }
    k_mutex_unlock(&nape_state_lock);
    if (pending) {
        LOG_INF("NAPE: yielding pending connection to split");
        err = bt_conn_disconnect(pending, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        bt_conn_unref(pending);
        if (err == -EALREADY || err == -ENOTCONN) err = 0;
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

#if IS_ENABLED(CONFIG_ZMK_NAPE_BOND_DIAGNOSTICS)
static void log_bond_address(const bt_addr_le_t *addr, uint8_t index, const char *kind) {
    LOG_INF("NAPE: %s[%u] %02X:%02X:%02X:%02X:%02X:%02X type=%u", kind, index,
            addr->a.val[5], addr->a.val[4], addr->a.val[3], addr->a.val[2], addr->a.val[1],
            addr->a.val[0], addr->type);
}

static void log_stored_bond(const struct bt_bond_info *info, void *user_data) {
    uint8_t *count = user_data;
    log_bond_address(&info->addr, (*count)++, "stored bond");
}

static void log_active_peer(struct bt_conn *conn, void *user_data) {
    struct bt_conn_info info;
    uint8_t *count = user_data;
    if (bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_LE ||
        info.state != BT_CONN_STATE_CONNECTED || !info.le.dst) {
        return;
    }
    log_bond_address(info.le.dst, (*count)++, "active peer");
}

static void log_bond_inventory(void) {
    uint8_t bonds = 0;
    uint8_t peers = 0;
    bt_foreach_bond(BT_ID_DEFAULT, log_stored_bond, &bonds);
    bt_conn_foreach(BT_CONN_TYPE_LE, log_active_peer, &peers);
    LOG_INF("NAPE: bond inventory stored=%u active LE=%u", bonds, peers);
}
#endif

#if IS_ENABLED(CONFIG_ZMK_NAPE_BOND_CLEANUP_ONLY)
#define NAPE_CLEANUP_BOND_TARGETS 3

/* FNV-1a fingerprints of the three Nape bond addresses found in the local
 * inventory capture. Keeping fingerprints here avoids publishing BLE MACs. */
static const uint64_t nape_cleanup_fingerprints[NAPE_CLEANUP_BOND_TARGETS] = {
    0x2e2d055e6f8cd73dULL,
    0xad902443e2ce384bULL,
    0xa8c670013f0ebdb6ULL,
};

struct bond_snapshot {
    bt_addr_le_t addresses[CONFIG_BT_MAX_PAIRED];
    uint8_t count;
    bool overflow;
};

struct peer_snapshot {
    bt_addr_le_t addresses[CONFIG_BT_MAX_CONN];
    uint8_t count;
    bool overflow;
};

static uint64_t nape_address_fingerprint(const bt_addr_le_t *addr) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    hash = (hash ^ addr->type) * 0x100000001b3ULL;
    for (size_t i = 0; i < sizeof(addr->a.val); i++) {
        hash = (hash ^ addr->a.val[i]) * 0x100000001b3ULL;
    }
    return hash;
}

static int nape_cleanup_fingerprint_index(uint64_t fingerprint) {
    for (int i = 0; i < NAPE_CLEANUP_BOND_TARGETS; i++) {
        if (fingerprint == nape_cleanup_fingerprints[i]) return i;
    }
    return -1;
}

static void capture_stored_bond(const struct bt_bond_info *info, void *user_data) {
    struct bond_snapshot *snapshot = user_data;
    if (snapshot->count >= ARRAY_SIZE(snapshot->addresses)) {
        snapshot->overflow = true;
        return;
    }
    bt_addr_le_copy(&snapshot->addresses[snapshot->count++], &info->addr);
}

static void capture_active_peer(struct bt_conn *conn, void *user_data) {
    struct peer_snapshot *snapshot = user_data;
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_LE ||
        info.state != BT_CONN_STATE_CONNECTED || !info.le.dst) {
        return;
    }
    if (snapshot->count >= ARRAY_SIZE(snapshot->addresses)) {
        snapshot->overflow = true;
        return;
    }
    bt_addr_le_copy(&snapshot->addresses[snapshot->count++], info.le.dst);
}

static bool snapshot_has_address(const struct bond_snapshot *snapshot,
                                 const bt_addr_le_t *address) {
    for (uint8_t i = 0; i < snapshot->count; i++) {
        if (bt_addr_le_cmp(&snapshot->addresses[i], address) == 0) return true;
    }
    return false;
}

static bool snapshot_has_active_peer(const struct peer_snapshot *snapshot,
                                     const bt_addr_le_t *address) {
    for (uint8_t i = 0; i < snapshot->count; i++) {
        if (bt_addr_le_cmp(&snapshot->addresses[i], address) == 0) return true;
    }
    return false;
}

static uint8_t count_stored_bonds(void) {
    struct bond_snapshot snapshot = {0};
    bt_foreach_bond(BT_ID_DEFAULT, capture_stored_bond, &snapshot);
    return snapshot.count;
}

static void cleanup_nape_bonds(void) {
    struct bond_snapshot bonds = {0};
    struct peer_snapshot peers = {0};
    bool targets_seen[NAPE_CLEANUP_BOND_TARGETS] = {false};
    uint8_t removed = 0;
    uint8_t failed = 0;

    bt_foreach_bond(BT_ID_DEFAULT, capture_stored_bond, &bonds);
    bt_conn_foreach(BT_CONN_TYPE_LE, capture_active_peer, &peers);

    /* Require both Cornix links, their corresponding stored keys, room for
     * both known unclassified bonds, and no duplicate target fingerprints. */
    bool safe = !bonds.overflow && bonds.count >= 4 &&
                bonds.count <= CONFIG_BT_MAX_PAIRED && !peers.overflow && peers.count == 2;
    for (uint8_t i = 0; safe && i < peers.count; i++) {
        if (!snapshot_has_address(&bonds, &peers.addresses[i])) safe = false;
    }
    for (uint8_t i = 0; safe && i < bonds.count; i++) {
        int target = nape_cleanup_fingerprint_index(
            nape_address_fingerprint(&bonds.addresses[i]));
        if (target < 0) continue;
        if (targets_seen[target] || snapshot_has_active_peer(&peers, &bonds.addresses[i])) {
            safe = false;
            break;
        }
        targets_seen[target] = true;
    }
    if (!safe) {
        LOG_ERR("NAPE: bond cleanup aborted; inventory guard failed (stored=%u active LE=%u)",
                bonds.count, peers.count);
        return;
    }

    for (uint8_t i = 0; i < bonds.count; i++) {
        int target = nape_cleanup_fingerprint_index(nape_address_fingerprint(&bonds.addresses[i]));
        if (target < 0) continue;
        int err = bt_unpair(BT_ID_DEFAULT, &bonds.addresses[i]);
        if (!err) {
            removed++;
            LOG_INF("NAPE: removed saved Nape bond candidate %u", target + 1);
        } else {
            failed++;
            LOG_ERR("NAPE: failed to remove saved Nape bond candidate %u (%d)", target + 1, err);
        }
    }

    LOG_INF("NAPE: bond cleanup complete removed=%u failed=%u remaining=%u active LE=%u",
            removed, failed, count_stored_bonds(), peers.count);
}
#endif

static void scan_work_handler(struct k_work *work) {
    k_mutex_lock(&nape_scan_lock, K_FOREVER);
    if (has_connection() || atomic_get(&bridge.connecting) || atomic_get(&bridge.scanning)) {
        k_mutex_unlock(&nape_scan_lock);
        return;
    }
    if (!zmk_split_ble_peripherals_ready()) {
        if (!waiting_for_split) {
            LOG_INF("NAPE: waiting for Cornix split discovery");
            waiting_for_split = true;
        }
        k_mutex_unlock(&nape_scan_lock);
        k_work_reschedule(&nape_scan_work, K_SECONDS(1));
        return;
    }
    waiting_for_split = false;
#if IS_ENABLED(CONFIG_ZMK_NAPE_BOND_CLEANUP_ONLY)
    k_mutex_unlock(&nape_scan_lock);
    if (atomic_cas(&bond_cleanup_started, 0, 1)) cleanup_nape_bonds();
    return;
#else
#if IS_ENABLED(CONFIG_ZMK_NAPE_BOND_DIAGNOSTICS)
    if (atomic_cas(&bond_inventory_logged, 0, 1)) log_bond_inventory();
#endif
    int err = bt_le_scan_start(BT_LE_SCAN_ACTIVE, device_found);
    if (!err) {
        atomic_set(&bridge.scanning, 1);
        atomic_clear(&bridge.candidate_ready);
        LOG_INF("NAPE: scan start");
        k_work_reschedule(&nape_scan_timeout_work, K_SECONDS(10));
    }
    k_mutex_unlock(&nape_scan_lock);
    if (err) {
        LOG_WRN("NAPE: scan start failed (%d)", err);
        schedule_scan_backoff();
    }
#endif
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
    struct bt_conn *pending = NULL;
    /* The HCI command may wait for Bluetooth RX; never hold the state lock here. */
    err = bt_conn_le_create(&bridge.candidate, BT_CONN_LE_CREATE_CONN,
                            BT_LE_CONN_PARAM_DEFAULT, &pending);
    k_mutex_lock(&nape_state_lock, K_FOREVER);
    if (err) {
        atomic_clear(&bridge.connecting);
    } else if (!bridge.conn && atomic_get(&bridge.connecting)) {
        bridge.conn = pending;
        pending = NULL;
    }
    k_mutex_unlock(&nape_state_lock);
    if (pending) bt_conn_unref(pending);
    k_mutex_unlock(&nape_scan_lock);
    if (err) schedule_scan_backoff();
}

static bool active_conn(struct bt_conn *conn) {
    k_mutex_lock(&nape_state_lock, K_FOREVER);
    bool active = conn && conn == bridge.conn;
    k_mutex_unlock(&nape_state_lock);
    return active;
}

static bool accept_pending_conn(struct bt_conn *conn) {
    k_mutex_lock(&nape_state_lock, K_FOREVER);
    if (!bridge.conn && atomic_get(&bridge.connecting) &&
        bt_addr_le_cmp(bt_conn_get_dst(conn), &bridge.candidate) == 0) {
        /* A connection-complete event can overtake the create call's return. */
        bridge.conn = bt_conn_ref(conn);
    }
    bool active = conn && conn == bridge.conn;
    k_mutex_unlock(&nape_state_lock);
    return active;
}

static uint8_t report_notify(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                             const void *data, uint16_t length) {
    if (!data) return BT_GATT_ITER_STOP;
    k_mutex_lock(&nape_state_lock, K_FOREVER);
    bool active = conn == bridge.conn;
    uint32_t generation = bridge.generation;
    k_mutex_unlock(&nape_state_lock);
    if (!active) return BT_GATT_ITER_CONTINUE;
    struct gatt_report *report = CONTAINER_OF(params, struct gatt_report, subscription);
    if (length > NAPE_MAX_NOTIFICATION) {
        LOG_WRN("NAPE: oversized input report %u", length);
        return BT_GATT_ITER_CONTINUE;
    }
    struct queued_input queued = {.generation = generation, .report_id = report->id,
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
                LOG_WRN("NAPE: ignoring HID reports beyond %u", NAPE_MAX_GATT_REPORTS);
                return BT_GATT_ITER_CONTINUE;
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
    if (!attr) {
        atomic_set(&bridge.services_scanned, 1);
        atomic_clear(&gatt_pending);
        LOG_ERR("NAPE: GATT discovery ended without HID service (%u services)",
                bridge.service_count);
        return BT_GATT_ITER_STOP;
    }
    if (!attr->user_data) {
        LOG_ERR("NAPE: service at handle %u has no value", attr->handle);
        return BT_GATT_ITER_CONTINUE;
    }
    const struct bt_gatt_service_val *service = attr->user_data;
    bridge.service_count++;
    if (service->uuid->type == BT_UUID_TYPE_16) {
        LOG_INF("NAPE: GATT service 0x%04x (%u-%u)", BT_UUID_16(service->uuid)->val,
                attr->handle, service->end_handle);
    } else if (service->uuid->type == BT_UUID_TYPE_128) {
        LOG_INF("NAPE: GATT service 128-bit (%u-%u)", attr->handle, service->end_handle);
        LOG_HEXDUMP_INF(BT_UUID_128(service->uuid)->val, 16, "NAPE: service UUID bytes");
    }
    if (bt_uuid_cmp(service->uuid, BT_UUID_HIDS)) return BT_GATT_ITER_CONTINUE;
    bridge.hid_start = attr->handle + 1;
    bridge.hid_end = service->end_handle;
    atomic_set(&bridge.services_scanned, 1);
    atomic_clear(&gatt_pending);
    LOG_INF("NAPE: HID service found (%u-%u)", bridge.hid_start, bridge.hid_end);
    k_work_submit(&nape_discovery_work);
    return BT_GATT_ITER_STOP;
}

static void discovery_work_handler(struct k_work *work) {
    k_mutex_lock(&nape_state_lock, K_FOREVER);
    if (!bridge.conn || bt_conn_get_security(bridge.conn) < BT_SECURITY_L2 ||
        (atomic_get(&bridge.services_scanned) && !bridge.hid_start) ||
        !atomic_cas(&gatt_pending, 0, 1)) {
        k_mutex_unlock(&nape_state_lock);
        return;
    }
    struct bt_conn *conn = bt_conn_ref(bridge.conn);
    memset(&bridge.discovery, 0, sizeof(bridge.discovery));
    if (!bridge.hid_start) {
        memset(bridge.reports, 0, sizeof(bridge.reports));
        memset(&bridge.parsed_map, 0, sizeof(bridge.parsed_map));
        bridge.service_count = 0;
        /* Enumerate services once so an absent HIDS UUID can be distinguished
         * from a failure in UUID-filtered discovery on the actual device. */
        bridge.discovery.uuid = NULL;
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

static void count_bond(const struct bt_bond_info *info, void *user_data) {
    ARG_UNUSED(info);
    uint8_t *count = user_data;
    (*count)++;
}

static void connected(struct bt_conn *conn, uint8_t err) {
    if (!accept_pending_conn(conn)) return;
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
    if (rc && rc != -EALREADY) {
        uint8_t bonds = 0;
        bt_foreach_bond(BT_ID_DEFAULT, count_bond, &bonds);
        LOG_ERR("NAPE: security request failed (%d), bonds %u/%u", rc, bonds,
                CONFIG_BT_MAX_PAIRED);
        bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
        return;
    }
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
    atomic_clear(&bridge.services_scanned);
    bridge.service_count = 0;
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
    if (err) {
        LOG_ERR("NAPE: scan arbitration registration failed (%d)", err);
        return err;
    }
    LOG_INF("NAPE: bridge initialized");
    k_work_reschedule(&nape_scan_work, K_SECONDS(1));
    return 0;
}
SYS_INIT(nape_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
