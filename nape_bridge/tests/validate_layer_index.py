#!/usr/bin/env python3
"""Check the Nape temporary and scroll layer wiring."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"Nape layer index validation failed: {message}", file=sys.stderr)
        raise SystemExit(1)


def layer_bindings(keymap: str, index: int) -> list[str]:
    layer = re.search(rf"(?ms)^\s*layer_{index}\s*\{{(.*?)^\s*\}};", keymap)
    require(layer is not None, f"layer {index} is missing")
    bindings = re.search(r"(?s)\bbindings\s*=\s*<(.*?)>;", layer.group(1))
    require(bindings is not None, f"layer {index} bindings are missing")
    return re.findall(r"&[A-Za-z0-9_]+(?:\s+[A-Za-z0-9_]+)?", bindings.group(1))


def main() -> None:
    header = (ROOT / "boards/shields/cornix_nape_bridge/nape_layer_index.h").read_text(
        encoding="utf-8"
    )
    overlay = (ROOT / "boards/shields/cornix_nape_bridge/cornix_nape_bridge.overlay").read_text(
        encoding="utf-8"
    )
    base = (ROOT / "config/cornix.keymap").read_text(encoding="utf-8")
    build = (ROOT / "build.yaml").read_text(encoding="utf-8")

    scroll_indices = re.findall(r"(?m)^\s*#define\s+NAPE_SCROLL_LAYER_INDEX\s+(\d+)\s*$", header)
    layer_indices = re.findall(r"(?m)^\s*#define\s+NAPE_MOUSE_LAYER_INDEX\s+(\d+)\s*$", header)
    timeouts = re.findall(r"(?m)^\s*#define\s+NAPE_MOUSE_LAYER_TIMEOUT_MS\s+(\d+)\s*$", header)
    scaler_numerators = re.findall(
        r"(?m)^\s*#define\s+NAPE_SCROLL_SCALER_NUMERATOR\s+(\d+)\s*$", header
    )
    scaler_denominators = re.findall(
        r"(?m)^\s*#define\s+NAPE_SCROLL_SCALER_DENOMINATOR\s+(\d+)\s*$", header
    )
    require(len(scroll_indices) == 1, "scroll layer index must have one numeric definition")
    require(len(layer_indices) == 1, "layer index must have one numeric definition")
    require(len(timeouts) == 1, "timeout must have one numeric definition")
    require(len(scaler_numerators) == 1, "scroll scaler numerator must be defined once")
    require(len(scaler_denominators) == 1, "scroll scaler denominator must be defined once")
    require("<&zip_temp_layer NAPE_MOUSE_LAYER_INDEX NAPE_MOUSE_LAYER_TIMEOUT_MS>" in overlay,
            "overlay must use the shared layer and timeout definitions")

    override = re.search(r"(?s)\bnape_scroll_mode\s*\{([^{}]*)\}", overlay)
    require(override is not None, "NAPE_SCROLL layer override is missing")
    require("layers = <NAPE_SCROLL_LAYER_INDEX>" in override.group(1),
            "scroll override must select NAPE_SCROLL layer")
    require("process-next" not in override.group(1),
            "scroll override must stop before the parent temporary-layer processor")
    processors = re.search(r"(?s)\binput-processors\s*=\s*(.*?);", override.group(1))
    require(processors is not None, "scroll override processor list is missing")
    processor_entries = re.findall(r"<&([A-Za-z0-9_]+)([^>]*)>", processors.group(1))
    require([entry[0] for entry in processor_entries] == [
        "zip_xy_to_scroll_mapper", "zip_scroll_scaler"
    ], "scroll override must map XY before scaling scroll events")
    require(processor_entries[1][1].split() == [
        "NAPE_SCROLL_SCALER_NUMERATOR", "NAPE_SCROLL_SCALER_DENOMINATOR"
    ], "scroll scaler must use the shared numerator and denominator")

    parent = re.search(r"(?ms)\bnape_motion_listener\s*\{(.*?)^ {4}\};", overlay)
    require(parent is not None and "<&zip_temp_layer NAPE_MOUSE_LAYER_INDEX NAPE_MOUSE_LAYER_TIMEOUT_MS>"
            in parent.group(1), "normal Nape motion must keep the 700 ms NAPE_MOUSE processor")
    controls = re.search(r"(?s)\bnape_controls_listener\s*\{([^{}]*)\}", overlay)
    require(controls is not None and "device = <&nape_controls>" in controls.group(1),
            "the independent button and physical-wheel listener must remain present")

    base_layers = re.findall(r'(?m)^\s*display-name\s*=\s*"([^"]+)"\s*;', base)
    require(len(base_layers) == 10, f"expected ten layers, found {len(base_layers)}")
    require(base_layers[8] == "NAPE_SCROLL", "existing layer 8 must be named NAPE_SCROLL")
    require(base_layers[9] == "NAPE_MOUSE", "existing layer 9 must be named NAPE_MOUSE")
    require(int(scroll_indices[0]) == 8,
            f"NAPE_SCROLL index is {scroll_indices[0]}, expected existing layer 8")
    require(int(layer_indices[0]) == 9,
            f"NAPE_MOUSE index is {layer_indices[0]}, expected existing layer 9")
    require(int(timeouts[0]) == 700,
            f"NAPE_MOUSE timeout is {timeouts[0]} ms, expected 700 ms")
    scroll_bindings = layer_bindings(base, 8)
    require(len(scroll_bindings) == 50, f"expected 50 Layer 8 bindings, found {len(scroll_bindings)}")
    require(all(binding == "&trans" for binding in scroll_bindings),
            "NAPE_SCROLL must be transparent at every key position")
    require(0 < int(scaler_numerators[0]) <= 32767,
            "scroll scaler numerator must fit the ZMK int16 scaler")
    require(0 < int(scaler_denominators[0]) <= 32767,
            "scroll scaler denominator must fit the ZMK int16 scaler")
    require("config/cornix_nape_bridge.keymap" not in build,
            "Nape builds must not use the old keymap that appends a layer")
    nape_builds = build.count("shield: cornix_dongle_adapter prospector_adapter cornix_nape_bridge")
    require(nape_builds > 0, "Nape build variants are missing")
    require(build.count("-DKEYMAP_FILE=$GITHUB_WORKSPACE/config/cornix.keymap") == nape_builds,
            "all Nape build variants must use the editor-visible keymap")

    print(
        "Nape layer validation: PASS "
        f"(NAPE_SCROLL index 8, scaler {scaler_numerators[0]}/{scaler_denominators[0]}; "
        f"NAPE_MOUSE index 9, timeout {timeouts[0]} ms)"
    )


if __name__ == "__main__":
    main()
