#!/usr/bin/env python3
"""Check that the temporary layer processor targets NAPE_MOUSE."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"Nape layer index validation failed: {message}", file=sys.stderr)
        raise SystemExit(1)


def main() -> None:
    header = (ROOT / "boards/shields/cornix_nape_bridge/nape_layer_index.h").read_text(
        encoding="utf-8"
    )
    overlay = (ROOT / "boards/shields/cornix_nape_bridge/cornix_nape_bridge.overlay").read_text(
        encoding="utf-8"
    )
    base = (ROOT / "config/cornix.keymap").read_text(encoding="utf-8")
    bridge = (ROOT / "config/cornix_nape_bridge.keymap").read_text(encoding="utf-8")

    layer_indices = re.findall(r"(?m)^\s*#define\s+NAPE_MOUSE_LAYER_INDEX\s+(\d+)\s*$", header)
    timeouts = re.findall(r"(?m)^\s*#define\s+NAPE_MOUSE_LAYER_TIMEOUT_MS\s+(\d+)\s*$", header)
    require(len(layer_indices) == 1, "layer index must have one numeric definition")
    require(len(timeouts) == 1, "timeout must have one numeric definition")
    require("<&zip_temp_layer NAPE_MOUSE_LAYER_INDEX NAPE_MOUSE_LAYER_TIMEOUT_MS>" in overlay,
            "overlay must use the shared layer and timeout definitions")

    base_layers = re.findall(r'(?m)^\s*display-name\s*=\s*"[^"]+"\s*;', base)
    bridge_layers = re.findall(r"(?m)^\s*([A-Za-z_][A-Za-z0-9_-]*_layer)\s*\{", bridge)
    require(bridge_layers == ["nape_mouse_layer"], "bridge keymap must append one Nape layer")
    require('display-name = "NAPE_MOUSE"' in bridge, "appended layer must be named NAPE_MOUSE")
    require(int(layer_indices[0]) == len(base_layers),
            f"NAPE_MOUSE index is {layer_indices[0]}, expected {len(base_layers)} after base layers")

    print(f"Nape temporary layer index: PASS (index {layer_indices[0]}, timeout {timeouts[0]} ms)")


if __name__ == "__main__":
    main()
