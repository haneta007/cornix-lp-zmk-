if(CONFIG_ZMK_NAPE_BRIDGE)
  zephyr_library()
  zephyr_library_sources(nape_bridge/bridge.c nape_bridge/hid_mouse.c)
endif()
