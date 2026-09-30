#!/usr/bin/env python3
"""Check that cursor inertia remains isolated from the Nape scroll path."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"Nape cursor validation failed: {message}", file=sys.stderr)
        raise SystemExit(1)


def node_body(source: str, node: str) -> str:
    match = re.search(
        rf"(?ms)^\s*{re.escape(node)}\s*\{{(.*?)^\s*\}};",
        source,
    )
    require(match is not None, f"node {node} is missing")
    return match.group(1)


def node_bindings(source: str, node: str) -> list[str]:
    body = node_body(source, node)
    match = re.search(r"(?s)\bbindings\s*=\s*<(.*?)>;", body)
    require(match is not None, f"{node} bindings are missing")
    return re.findall(r"&[A-Za-z0-9_]+(?:\s+[A-Za-z0-9_]+)?", match.group(1))


def node_sensor_bindings(source: str, node: str) -> str:
    body = node_body(source, node)
    match = re.search(r"(?s)\bsensor-bindings\s*=\s*<(.*?)>;", body)
    require(match is not None, f"{node} sensor bindings are missing")
    return " ".join(match.group(1).split())


def main() -> None:
    header = (ROOT / "boards/shields/cornix_nape_bridge/nape_layer_index.h").read_text(
        encoding="utf-8"
    )
    overlay = (ROOT / "boards/shields/cornix_nape_bridge/cornix_nape_bridge.overlay").read_text(
        encoding="utf-8"
    )
    keymap = (ROOT / "config/cornix.keymap").read_text(encoding="utf-8")
    build = (ROOT / "build.yaml").read_text(encoding="utf-8")
    west = (ROOT / "config/west.yml").read_text(encoding="utf-8")
    inertia = (ROOT / "nape_bridge/inertia.c").read_text(encoding="utf-8")
    math = (ROOT / "nape_bridge/inertia_math.h").read_text(encoding="utf-8")
    bridge = (ROOT / "nape_bridge/bridge.c").read_text(encoding="utf-8")
    input_queue = (ROOT / "nape_bridge/input_queue.h").read_text(encoding="utf-8")

    expected_layer_constants = {
        "NAPE_SCROLL_LAYER_INDEX": 5,
        "NAPE_MOUSE_LAYER_INDEX": 6,
        "NAPE_SCROLL_COMPAT_LAYER_INDEX": 7,
        "NAPE_MOUSE_LAYER_TIMEOUT_MS": 700,
        "NAPE_SCROLL_SCALER_NUMERATOR": 1,
        "NAPE_SCROLL_SCALER_DENOMINATOR": 8,
    }
    for name, expected in expected_layer_constants.items():
        values = re.findall(rf"(?m)^\s*#define\s+{name}\s+(\d+)\s*$", header)
        require(len(values) == 1 and int(values[0]) == expected,
                f"{name} must remain {expected}")

    motion_listener = re.search(
        r"(?ms)^\s*nape_motion_listener\s*\{(.*?)^    \};", overlay
    )
    require(motion_listener is not None, "normal Nape listener is missing")
    require(
        "<&zip_temp_layer NAPE_MOUSE_LAYER_INDEX NAPE_MOUSE_LAYER_TIMEOUT_MS>"
        in motion_listener.group(1),
        "real Nape motion must keep the 700 ms mouse layer",
    )
    scroll_override = re.search(r"(?s)\bnape_scroll_mode\s*\{([^{}]*)\}", overlay)
    require(scroll_override is not None, "FN_SCROLL override is missing")
    require("layers = <NAPE_SCROLL_LAYER_INDEX>" in scroll_override.group(1),
            "scroll override must continue targeting Layer 5")
    require("process-next" not in scroll_override.group(1),
            "scroll override must not re-enter the parent temporary-layer processor")
    scroll_list = re.search(r"(?s)\binput-processors\s*=\s*(.*?);",
                            scroll_override.group(1))
    require(scroll_list is not None, "scroll processor list is missing")
    scroll_processors = re.findall(r"<&([A-Za-z0-9_]+)([^>]*)>", scroll_list.group(1))
    require([item[0] for item in scroll_processors] ==
            ["zip_xy_to_scroll_mapper", "zip_scroll_scaler"],
            "the existing XY mapper and 1/8 scaler must remain the scroll path")
    require(scroll_processors[-1][1].split() ==
            ["NAPE_SCROLL_SCALER_NUMERATOR", "NAPE_SCROLL_SCALER_DENOMINATOR"],
            "scroll must use the shared 1/8 scaler")

    require(re.search(
        r'(?s)nape_inertia_cursor:\s*nape_inertia_cursor\s*\{\s*'
        r'compatible\s*=\s*"nape,virtual-pointer"', overlay) is not None,
        "cursor inertia must use its own virtual pointer")
    processor_node = re.search(
        r'(?s)nape_cursor_inertia:\s*nape_cursor_inertia\s*\{([^}]*)\}', overlay
    )
    require(processor_node is not None and
            'compatible = "nape,input-processor-inertia"' in processor_node.group(1) and
            "#input-processor-cells = <0>;" in processor_node.group(1),
            "the cursor event gate processor must be declared with zero cells")
    inertia_listener = re.search(
        r"(?s)\bnape_cursor_inertia_listener\s*\{([^{}]*)\}", overlay
    )
    require(inertia_listener is not None and
            "device = <&nape_inertia_cursor>" in inertia_listener.group(1),
            "synthetic cursor motion needs an independent listener")
    listener_processors = re.search(
        r"(?s)\binput-processors\s*=\s*(.*?);", inertia_listener.group(1)
    )
    require(listener_processors is not None and
            re.findall(r"<&([A-Za-z0-9_]+)", listener_processors.group(1)) ==
            ["nape_cursor_inertia"],
            "synthetic motion must pass only through its stale-event gate")
    require("zip_temp_layer" not in inertia_listener.group(1) and
            "zip_xy_to_scroll_mapper" not in inertia_listener.group(1) and
            "zip_scroll_scaler" not in inertia_listener.group(1),
            "synthetic cursor motion must not refresh the mouse layer or enter scroll")

    expected_motion_constants = {
        "NAPE_CURSOR_ACCEL_START_COUNTS": 2,
        "NAPE_CURSOR_ACCEL_FULL_COUNTS": 12,
        "NAPE_CURSOR_ACCEL_BASE_Q8": 256,
        "NAPE_CURSOR_ACCEL_MAX_Q8": 320,
        "NAPE_CURSOR_INERTIA_START_DELAY_MS": 48,
        "NAPE_CURSOR_INERTIA_TICK_MS": 16,
        "NAPE_CURSOR_INERTIA_DECAY_NUMERATOR": 192,
        "NAPE_CURSOR_INERTIA_DECAY_DENOMINATOR": 256,
        "NAPE_CURSOR_INERTIA_MIN_START_COUNTS_PER_TICK": 12,
        "NAPE_CURSOR_INERTIA_MIN_REPORTS": 2,
        "NAPE_CURSOR_INERTIA_MIN_RAW_COUNTS": 24,
        "NAPE_CURSOR_INERTIA_SAMPLE_WINDOW_MS": 80,
        "NAPE_CURSOR_INERTIA_STOP_VELOCITY_Q8": 128,
        "NAPE_CURSOR_INERTIA_MAX_DURATION_MS": 224,
        "NAPE_CURSOR_INERTIA_MAX_IDLE_MS": 350,
    }
    for name, expected in expected_motion_constants.items():
        values = re.findall(rf"(?m)^\s*#define\s+{name}\s+(\d+)\s*$", math)
        require(len(values) == 1 and int(values[0]) == expected,
                f"{name} must remain {expected}")

    require("nape_cursor_accel_scale(raw_x" in inertia and
            "nape_cursor_accel_scale(raw_y" in inertia,
            "normal pointer movement must use per-axis acceleration")
    require("nape_scroll_axis_filter_process(&inertia_state.axis_filter" in inertia,
            "Layer 5 must retain the existing scroll-axis filter")
    require("nape_cursor_inertia_history_prune(&inertia_state.history," in inertia and
            "inertia_state.last_input_ms);" in inertia,
            "the flick gate must evaluate the 80 ms window at the last real input")
    require("input_report_rel(dev, INPUT_REL_X" in inertia and
            "input_report_rel(dev, INPUT_REL_Y" in inertia and
            "INPUT_REL_WHEEL" not in inertia and "INPUT_REL_HWHEEL" not in inertia,
            "the inertia generator must emit cursor axes only")
    require("zmk_keymap_layer_active(scroll_layer_id())" in inertia and
            "scroll_layer_active() || !atomic_get(&inertia_armed)" in inertia,
            "queued synthetic cursor events must be discarded when Layer 5 is active")
    require("zmk/events/position_state_changed.h" in inertia and
            "ZMK_SUBSCRIPTION(nape_cursor_inertia_position, zmk_position_state_changed)" in inertia and
            "if (state && state->state)" in inertia,
            "any Cornix physical key press must cancel inertia, including layer-only keys")
    require(re.search(
        r"(?s)state->layer\s*==\s*scroll_layer_id\(\)\s*\)\s*\{\s*"
        r"nape_inertia_reset\(\);", inertia) is not None,
        "Layer 5 activation and release must clear pointer inertia")
    require("k_work_reschedule(&inertia_work, K_MSEC(NAPE_CURSOR_INERTIA_START_DELAY_MS))"
            in inertia and "K_WORK_DELAYABLE_DEFINE(inertia_work" in inertia,
            "cursor inertia must reuse one system delayable work item")
    require("nape_inertia_cancel();" in bridge and
            "const uint8_t next_held_buttons" in bridge and
            "const bool controls_active = next_held_buttons != 0" in bridge,
            "buttons and wheel, including buttons reported separately from XY, must cancel inertia")
    require("nape_inertia_reset();" in bridge,
            "Nape disconnect must reset all motion state")
    require("nape_cursor_inertia_pack_event" in inertia and
            "nape_cursor_inertia_event_is_current" in inertia,
            "queued synthetic cursor motion must be epoch guarded")
    require("NAPE_SCROLL_INERTIA_" not in math and
            "nape_scroll_inertia" not in overlay and
            "nape_inertia_scroll" not in overlay,
            "scroll inertia must be removed")

    require("uint32_t received_ms;" in input_queue,
            "queued BLE reports must preserve arrival timestamps")
    notify = re.search(r"(?s)static uint8_t report_notify\(.*?\n}\n", bridge)
    require(notify is not None and ".received_ms = k_uptime_get_32()" in notify.group(0),
            "BLE arrival time must be recorded before report queueing")
    disconnect = re.search(r"(?s)static void disconnected\(.*?\n}\n", bridge)
    require(disconnect is not None, "Nape disconnect callback is missing")
    disconnect_body = disconnect.group(0)
    generation_update = disconnect_body.find("bridge.generation++")
    inertia_reset = disconnect_body.find("nape_inertia_reset();")
    final_unlock = disconnect_body.rfind("k_mutex_unlock(&nape_state_lock)")
    require(0 <= generation_update < inertia_reset < final_unlock,
            "disconnect must invalidate queued reports before clearing inertia")

    input_work = re.search(
        r"(?ms)^static void input_work_handler\(struct k_work \*work\) \{(.*?)^\}",
        bridge,
    )
    input_body = input_work.group(1) if input_work else ""
    stages = [
        input_body.find("queued.generation != bridge.generation"),
        input_body.find("nape_hid_parse_input"),
        input_body.find("nape_inertia_prepare_motion(parsed.x, parsed.y, queued.received_ms"),
        input_body.find("emit_relative(motion, INPUT_REL_X, motion_x"),
    ]
    require(all(position >= 0 for position in stages) and stages == sorted(stages),
            "parsed XY must be tracked before the normal pointer listener")

    filter_header = (ROOT / "nape_bridge/scroll_filter.h").read_text(encoding="utf-8")
    require("NAPE_SCROLL_X_AXIS_DOMINANCE_RATIO 2" in filter_header and
            "NAPE_SCROLL_VERTICAL_AXIS_LOCK_MS 64" in filter_header and
            "NAPE_SCROLL_X_AXIS_THRESHOLD_COUNTS 4" in filter_header and
            "motion->scroll_y = nape_scroll_axis_negate(raw_y);" in filter_header,
            "the existing horizontal threshold and vertical inversion must stay intact")

    names = re.findall(r'(?m)^\s*display-name\s*=\s*"([^"]+)"\s*;', keymap)
    require(len(names) == 10 and names[5:8] ==
            ["FN_SCROLL", "NAPE_MOUSE", "FN_SCROLL_COMPAT"],
            "Layers 5-7 must remain FN_SCROLL, NAPE_MOUSE, and compatibility")
    for index, expected_name in ((5, "FN_SCROLL"), (6, "NAPE_MOUSE"),
                                 (7, "FN_SCROLL_COMPAT")):
        require(f'display-name = "{expected_name}";' in node_body(keymap, f"layer_{index}"),
                f"Layer {index} display name must remain {expected_name}")
    require(len(node_bindings(keymap, "fn_layer")) == 50 and
            node_bindings(keymap, "layer_5") == node_bindings(keymap, "fn_layer"),
            "Layer 5 must retain the existing FN keyboard bindings")
    require(node_sensor_bindings(keymap, "layer_5") ==
            node_sensor_bindings(keymap, "fn_layer"),
            "Layer 5 must retain the existing FN sensor bindings")
    require(len(node_bindings(keymap, "layer_6")) == 50,
            "Layer 6 mouse bindings must remain intact")
    mouse_bindings = node_bindings(keymap, "layer_6")
    require(mouse_bindings[19:22] == ["&mkp MB1", "&mkp MB3", "&mkp MB2"] and
            mouse_bindings[30:32] == ["&kp C_MUTE", "&mkp MB3"],
            "Layer 6 mouse buttons and mute binding must remain intact")
    compat_bindings = node_bindings(keymap, "layer_7")
    require(len(compat_bindings) == 50 and
            compat_bindings[19:22] == ["&kp N5", "&kp N6", "&kp KP_PLUS"] and
            all(binding == "&trans" for index, binding in enumerate(compat_bindings)
                if index not in (19, 20, 21)),
            "Layer 7 must resolve only the three Layer 6/FN key conflicts")
    require(all(binding == "&trans" for layer in (8, 9)
                for binding in node_bindings(keymap, f"layer_{layer}")),
            "legacy Layers 8 and 9 must remain transparent")
    require("&lt150 FN_SCROLL INT_MUHENKAN" in node_body(keymap, "base_layer") and
            "&lt150 FN_SCROLL ENTER" in node_body(keymap, "base_layer"),
            "both existing FN tap-holds must still activate Layer 5")
    hold_tap = re.search(
        r"(?sm)^\s*lt150:\s*layer_tap_150\s*\{(.*?)^\s*\};", keymap
    )
    require(hold_tap is not None and 'flavor = "hold-preferred";' in hold_tap.group(1) and
            "tapping-term-ms = <150>;" in hold_tap.group(1) and
            "quick-tap-ms = <150>;" in hold_tap.group(1),
            "the existing 150 ms tap-hold timing must remain unchanged")
    conditional = re.search(r"(?s)\bconditional_layers\s*\{([^{}]*\{.*?\}[^{}]*)\}", keymap)
    require(conditional is not None and
            "if-layers = <FN_SCROLL NAPE_MOUSE>;" in conditional.group(1) and
            "then-layer = <FN_SCROLL_COMPAT>;" in conditional.group(1),
            "Layer 7 must activate only when Layers 5 and 6 are active")

    for function_name, required_order in (
        ("nape_inertia_reset", ["k_spin_lock", "cancel_locked", "reset_locked",
                                "k_spin_unlock"]),
        ("nape_inertia_prepare_motion", ["k_spin_lock", "k_work_reschedule",
                                         "k_spin_unlock"]),
        ("reset_if_epoch", ["k_spin_lock", "k_work_cancel_delayable", "k_spin_unlock"]),
    ):
        function = re.search(
            rf"(?s)(?:static\s+)?void\s+{function_name}\s*\([^)]*\)\s*\{{(.*?)\n\}}",
            inertia,
        )
        require(function is not None, f"{function_name} implementation is missing")
        positions = [
            function.group(1).rfind(token) if index == len(required_order) - 1
            else function.group(1).find(token)
            for index, token in enumerate(required_order)
        ]
        require(all(position >= 0 for position in positions) and
                positions == sorted(positions),
                f"{function_name} must serialize state and work operations")

    require("revision: edafb3b058445329d4cbc226621eb1d37529480c" in west and
            "revision: 28438c476e2e17d648ce25a14d85525997cc48e3" in west,
            "pinned ZMK and Prospector module commits must not change")
    nape_variants = build.count("shield: cornix_dongle_adapter prospector_adapter cornix_nape_bridge")
    require(nape_variants >= 5 and
            build.count("-DKEYMAP_FILE=$GITHUB_WORKSPACE/config/cornix.keymap") == nape_variants,
            "all Nape firmware variants must keep the shared editor-visible keymap")
    require("artifact-name: cornix_prospector_nape_bridge_nosd" in build,
            "the normal Nape firmware artifact name must remain unchanged")

    print("Nape cursor validation: PASS (Layer 5 scroll preserved; cursor acceleration/inertia isolated)")


if __name__ == "__main__":
    main()
