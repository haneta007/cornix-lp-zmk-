#!/usr/bin/env python3
"""Check the Nape/FN layer migration and scroll input wiring."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"Nape layer validation failed: {message}", file=sys.stderr)
        raise SystemExit(1)


def node_body(source: str, node: str) -> str:
    match = re.search(rf"(?ms)^        {re.escape(node)}\s*\{{(.*?)^        \}};", source)
    require(match is not None, f"keymap node {node} is missing")
    return match.group(1)


def layer_bindings(source: str, index: int) -> list[str]:
    return node_bindings(source, f"layer_{index}")


def node_bindings(source: str, node: str) -> list[str]:
    layer = node_body(source, node)
    bindings = re.search(r"(?s)\bbindings\s*=\s*<(.*?)>;", layer)
    require(bindings is not None, f"layer node {node} bindings are missing")
    return re.findall(r"&[A-Za-z0-9_]+(?:\s+[A-Za-z0-9_]+)?", bindings.group(1))


def sensor_bindings(source: str, node: str) -> str | None:
    match = re.search(r"(?s)\bsensor-bindings\s*=\s*<(.*?)>;", node_body(source, node))
    return match.group(1).strip() if match else None


def main() -> None:
    header = (ROOT / "boards/shields/cornix_nape_bridge/nape_layer_index.h").read_text(
        encoding="utf-8"
    )
    overlay = (ROOT / "boards/shields/cornix_nape_bridge/cornix_nape_bridge.overlay").read_text(
        encoding="utf-8"
    )
    keymap = (ROOT / "config/cornix.keymap").read_text(encoding="utf-8")
    build = (ROOT / "build.yaml").read_text(encoding="utf-8")
    inertia = (ROOT / "nape_bridge/inertia.c").read_text(encoding="utf-8")
    bridge = (ROOT / "nape_bridge/bridge.c").read_text(encoding="utf-8")
    input_queue = (ROOT / "nape_bridge/input_queue.h").read_text(encoding="utf-8")

    expected_constants = {
        "NAPE_SCROLL_LAYER_INDEX": 5,
        "NAPE_MOUSE_LAYER_INDEX": 6,
        "NAPE_SCROLL_COMPAT_LAYER_INDEX": 7,
        "NAPE_MOUSE_LAYER_TIMEOUT_MS": 700,
        "NAPE_SCROLL_SCALER_NUMERATOR": 1,
        "NAPE_SCROLL_SCALER_DENOMINATOR": 8,
    }
    for name, expected in expected_constants.items():
        values = re.findall(rf"(?m)^\s*#define\s+{name}\s+(\d+)\s*$", header)
        require(len(values) == 1, f"{name} must have one numeric definition")
        require(int(values[0]) == expected, f"{name} is {values[0]}, expected {expected}")

    require('<&zip_temp_layer NAPE_MOUSE_LAYER_INDEX NAPE_MOUSE_LAYER_TIMEOUT_MS>' in overlay,
            "normal Nape motion must keep its 700 ms temporary mouse layer")
    require(re.search(r"(?s)nape_scroll_inertia:\s*nape_scroll_inertia\s*\{\s*compatible\s*=\s*\"nape,input-processor-inertia\"", overlay) is not None,
            "the custom tracker input processor must be declared in the shield overlay")
    require(re.search(r"(?s)nape_scroll_inertia:\s*nape_scroll_inertia\s*\{[^}]*#input-processor-cells\s*=\s*<0>;", overlay) is not None,
            "the zero-parameter tracker binding must declare zero input-processor cells")
    require(re.search(r"(?s)nape_inertia_scroll:\s*nape_inertia_scroll\s*\{\s*compatible\s*=\s*\"nape,virtual-pointer\"", overlay) is not None,
            "synthetic inertia must use a separate virtual input device")
    scroll_override = re.search(r"(?s)\bnape_scroll_mode\s*\{([^{}]*)\}", overlay)
    require(scroll_override is not None, "scroll-layer input override is missing")
    require("layers = <NAPE_SCROLL_LAYER_INDEX>" in scroll_override.group(1),
            "scroll override must target FN_SCROLL / Layer 5")
    require("process-next" not in scroll_override.group(1),
            "scroll override must bypass the parent temporary-layer processor")
    processors = re.search(r"(?s)\binput-processors\s*=\s*(.*?);", scroll_override.group(1))
    require(processors is not None, "scroll override processor list is missing")
    processor_entries = re.findall(r"<&([A-Za-z0-9_]+)([^>]*)>", processors.group(1))
    require([entry[0] for entry in processor_entries] == [
        "zip_xy_to_scroll_mapper", "zip_scroll_scaler"
    ], "normal scroll must retain the standard XY mapper and scaler")
    require(processor_entries[-1][1].split() == [
        "NAPE_SCROLL_SCALER_NUMERATOR", "NAPE_SCROLL_SCALER_DENOMINATOR"
    ], "scroll override must use the shared 1/8 scaler values")

    inertia_listener = re.search(r"(?s)\bnape_inertia_listener\s*\{([^{}]*)\}", overlay)
    require(inertia_listener is not None and "device = <&nape_inertia_scroll>" in inertia_listener.group(1),
            "synthetic inertia events need an independent listener")
    inertia_processor_list = re.search(
        r"(?s)\binput-processors\s*=\s*(.*?);", inertia_listener.group(1)
    )
    require(inertia_processor_list is not None, "inertia listener processor list is missing")
    inertia_processors = re.findall(r"<&([A-Za-z0-9_]+)([^>]*)>", inertia_processor_list.group(1))
    require([entry[0] for entry in inertia_processors] == ["nape_scroll_inertia", "zip_scroll_scaler"],
            "inertia listener must gate and scale synthetic scroll only")
    require("zip_temp_layer" not in inertia_listener.group(1) and
            "zip_xy_to_scroll_mapper" not in inertia_listener.group(1),
            "inertia events must not reach the mouse-layer timer or XY mapper")
    require('#include "nape_layer_index.h"' in inertia and "#define NAPE_SCROLL_LAYER_INDEX" not in inertia,
            "inertia must share the layer index header rather than duplicate the value")
    require("nape_inertia_reset();" in bridge,
            "Nape disconnect must clear inertia state")
    require("uint32_t received_ms;" in input_queue,
            "the fixed input queue must carry each BLE notification timestamp")
    notify = re.search(r"(?s)static uint8_t report_notify\(.*?\n}\n", bridge)
    require(notify is not None and ".received_ms = k_uptime_get_32()" in notify.group(0),
            "BLE notification arrival time must be recorded before queueing")
    disconnect = re.search(r"(?s)static void disconnected\(.*?\n}\n", bridge)
    require(disconnect is not None, "the Nape disconnect callback is missing")
    disconnect_body = disconnect.group(0)
    generation_update = disconnect_body.find("bridge.generation++")
    inertia_reset = disconnect_body.find("nape_inertia_reset();")
    final_unlock = disconnect_body.rfind("k_mutex_unlock(&nape_state_lock)")
    require(0 <= generation_update < inertia_reset < final_unlock,
            "disconnect must invalidate the connection and reset inertia under the input-state mutex")
    input_work = re.search(
        r"(?ms)^static void input_work_handler\(struct k_work \*work\) \{(.*?)^\}", bridge
    )
    input_body = input_work.group(1) if input_work is not None else ""
    input_stages = [
        input_body.find("queued.generation != bridge.generation"),
        input_body.find("nape_hid_parse_input"),
        input_body.find("nape_inertia_prepare_motion(parsed.x, parsed.y, queued.received_ms"),
        input_body.find("emit_relative(motion, INPUT_REL_X, motion_x"),
    ]
    require(all(position >= 0 for position in input_stages) and
            input_stages == sorted(input_stages),
            "parsed XY must be filtered before ordinary scroll routing and inertia tracking")
    filter_header = (ROOT / "nape_bridge/scroll_filter.h").read_text(encoding="utf-8")
    require("NAPE_SCROLL_X_AXIS_DOMINANCE_RATIO 2" in filter_header and
            "NAPE_SCROLL_VERTICAL_AXIS_LOCK_MS 64" in filter_header and
            "NAPE_SCROLL_X_AXIS_THRESHOLD_COUNTS 4" in filter_header,
            "horizontal filtering must use the agreed dominance ratio, lock, and threshold")
    require("nape_scroll_axis_filter_process" in inertia and
            "nape_scroll_axis_filter_reset(&inertia_state.axis_filter);" in inertia,
            "axis filtering must share inertia lifecycle and reset state")
    require("motion->scroll_y = nape_scroll_axis_negate(raw_y);" in filter_header,
            "ball-derived vertical scroll must be inverted before ordinary and inertia routing")
    require("k_work_cancel_delayable(&inertia_work);" in inertia and
            "k_work_reschedule(&inertia_work, K_MSEC(NAPE_SCROLL_INERTIA_START_DELAY_MS));" in inertia,
            "reset cancellation and input rescheduling must both remain present")
    reset = re.search(r"(?s)void nape_inertia_reset\(void\)\s*\{(.*?)\n}", inertia)
    reset_positions = [
        reset.group(1).find(token) if reset is not None else -1
        for token in ("k_spin_lock", "k_work_cancel_delayable", "k_spin_unlock")
    ]
    require(all(position >= 0 for position in reset_positions) and
            reset_positions == sorted(reset_positions),
            "reset and delayed-work cancellation must be serialized by the inertia state lock")
    prepare = re.search(r"(?s)void nape_inertia_prepare_motion\(.*?\)\s*\{(.*?)\n}", inertia)
    prepare_body = prepare.group(1) if prepare is not None else ""
    prepare_schedule = [
        prepare_body.find("k_spin_lock"), prepare_body.find("k_work_reschedule"),
        prepare_body.rfind("k_spin_unlock")
    ]
    require(all(position >= 0 for position in prepare_schedule) and
            prepare_schedule == sorted(prepare_schedule),
            "input rescheduling must be serialized against state reset")
    reset_epoch = re.search(r"(?s)static void reset_if_epoch\(.*?\)\s*\{(.*?)\n}", inertia)
    reset_epoch_positions = [
        reset_epoch.group(1).find(token) if reset_epoch is not None else -1
        for token in ("k_spin_lock", "k_work_cancel_delayable", "k_spin_unlock")
    ]
    require(all(position >= 0 for position in reset_epoch_positions) and
            reset_epoch_positions == sorted(reset_epoch_positions),
            "epoch-guarded cancellation must be serialized by the inertia state lock")
    work_calls = list(re.finditer(r"k_work_(?:cancel_delayable|reschedule)\s*\(", inertia))
    work_handler = re.search(
        r"(?ms)^static void inertia_work_handler\(struct k_work \*work\) \{(.*?)^\}", inertia
    )
    handler_body = work_handler.group(1) if work_handler is not None else ""
    handler_calls = list(re.finditer(r"k_work_reschedule\s*\(", handler_body))
    handler_calls_locked = all(
        handler_body.rfind("k_spin_lock", 0, call.start()) >
        handler_body.rfind("k_spin_unlock", 0, call.start()) and
        handler_body.find("k_spin_unlock", call.end()) >= call.end()
        for call in handler_calls
    )
    require(len(work_calls) == 6 and len(handler_calls) == 2 and handler_calls_locked,
            "all inertia work scheduling/cancel paths must stay serialized by the state lock")
    require("if (event->dev == NAPE_INPUT_NODE(nape_motion))" not in inertia,
            "inertia sampling must not depend on asynchronously delivered motion input events")
    two_axis_emit = re.search(r"(?s)if \(hwheel != 0 && wheel != 0\) \{(.*?)\n    \} else if", inertia)
    require(two_axis_emit is not None and
            len(re.findall(r"input_report_rel\(.*?true, K_NO_WAIT\)", two_axis_emit.group(1), re.S)) == 2,
            "each axis of a diagonal inertia tick must close its input report independently")
    require(re.search(r"(?s)state->layer\s*==\s*scroll_layer_id\(\).*?!state->state", inertia) is not None,
            "releasing Layer 5 must cancel pending inertia")
    require("atomic_inc(&inertia_epoch);" in inertia and
            "nape_inertia_scroll_event_is_current" in inertia,
            "new motion, layer release, and disconnect must invalidate queued synthetic scroll")

    motion_listener = re.search(r"(?ms)\bnape_motion_listener\s*\{(.*?)^    \};", overlay)
    require(motion_listener is not None and
            "<&zip_temp_layer NAPE_MOUSE_LAYER_INDEX NAPE_MOUSE_LAYER_TIMEOUT_MS>" in motion_listener.group(1),
            "normal pointer motion must retain the Layer 6 / 700 ms behavior")
    require("nape_controls_listener" in overlay and "device = <&nape_controls>" in overlay,
            "buttons and physical wheel must retain their separate input listener")

    names = re.findall(r'(?m)^\s*display-name\s*=\s*"([^"]+)"\s*;', keymap)
    require(len(names) == 10, f"layer count must stay at ten, found {len(names)}")
    require(names[5:10] == ["FN_SCROLL", "NAPE_MOUSE", "FN_SCROLL_COMPAT", "Legacy 8", "Legacy 9"],
            "Layers 5–9 must use the migrated labels while retaining legacy slots 8/9")
    for name, expected in (("FN_SCROLL", 5), ("NAPE_MOUSE", 6), ("FN_SCROLL_COMPAT", 7)):
        values = re.findall(rf"(?m)^#define\s+{name}\s+(\d+)\s*$", keymap)
        require(len(values) == 1 and int(values[0]) == expected,
                f"keymap symbol {name} must map to existing Layer {expected}")
    for index, expected_name in ((5, "FN_SCROLL"), (6, "NAPE_MOUSE"), (7, "FN_SCROLL_COMPAT")):
        require(node_body(keymap, f"layer_{index}").find(f'display-name = "{expected_name}";') >= 0,
                f"Layer {index} display name must be {expected_name}")

    fn_bindings = node_bindings(keymap, "fn_layer")
    scroll_bindings = layer_bindings(keymap, 5)
    require(len(fn_bindings) == 50 and scroll_bindings == fn_bindings,
            "Layer 5 keyboard bindings must match the existing Layer 1 FN layout")
    require(sensor_bindings(keymap, "fn_layer") == sensor_bindings(keymap, "layer_5"),
            "Layer 5 sensor bindings must preserve the existing FN controls")
    require(len(layer_bindings(keymap, 6)) == 50, "Layer 6 must retain all 50 mouse-layer positions")
    mouse_bindings = layer_bindings(keymap, 6)
    require(mouse_bindings[19:22] == ["&mkp MB1", "&mkp MB3", "&mkp MB2"] and
            mouse_bindings[30:32] == ["&kp C_MUTE", "&mkp MB3"],
            "Layer 6 mouse buttons and existing mute binding must be preserved")
    compat_bindings = layer_bindings(keymap, 7)
    require(len(compat_bindings) == 50 and
            [compat_bindings[i] for i in range(19, 22)] == ["&kp N5", "&kp N6", "&kp KP_PLUS"],
            "Layer 7 must keep the three FN bindings that overlap Layer 6 mouse buttons")
    require(all(binding == "&trans" for i, binding in enumerate(compat_bindings) if i not in (19, 20, 21)),
            "Layer 7 must be transparent outside the three key conflicts")
    require(all(binding == "&trans" for index in (8, 9) for binding in layer_bindings(keymap, index)),
            "legacy Layers 8 and 9 must remain present but inert")

    base = node_body(keymap, "base_layer")
    require(base.count("&lt150 FN_SCROLL INT_MUHENKAN") == 1 and
            base.count("&lt150 FN_SCROLL ENTER") == 1,
            "both Base-layer FN tap-holds must now activate Layer 5")
    require("&lt150 LAYER8" not in keymap and "&mo 8" not in keymap,
            "no existing FN/scroll entry may still target old Layer 8")
    hold_tap = re.search(r"(?sm)^        lt150:\s*layer_tap_150\s*\{(.*?)^        \};", keymap)
    require(hold_tap is not None and 'flavor = "hold-preferred";' in hold_tap.group(1) and
            "tapping-term-ms = <150>;" in hold_tap.group(1) and
            "quick-tap-ms = <150>;" in hold_tap.group(1),
            "the existing 150 ms FN tap-hold behavior must remain unchanged")

    conditional = re.search(r"(?s)\bconditional_layers\s*\{([^{}]*\{.*?\}[^{}]*)\}", keymap)
    require(conditional is not None and "if-layers = <FN_SCROLL NAPE_MOUSE>;" in conditional.group(1) and
            "then-layer = <FN_SCROLL_COMPAT>;" in conditional.group(1),
            "Layer 7 must activate only while Layers 5 and 6 are both active")

    require("config/cornix_nape_bridge.keymap" not in build,
            "Nape builds must use the Keymap Editor-visible common keymap")
    nape_builds = build.count("shield: cornix_dongle_adapter prospector_adapter cornix_nape_bridge")
    require(nape_builds > 0, "Nape firmware build variants are missing")
    require(build.count("-DKEYMAP_FILE=$GITHUB_WORKSPACE/config/cornix.keymap") == nape_builds,
            "all Nape firmware variants must use config/cornix.keymap")

    print("Nape layer validation: PASS (FN_SCROLL 5, NAPE_MOUSE 6, compatibility 7, legacy 8/9 retained)")


if __name__ == "__main__":
    main()
