if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  zephyr_include_directories(include)

  # The modular stack: the pure core (dm_render / slot_store / dm_machine /
  # dm_feedback_build / dm_query) plus the Zephyr shells (dm_feedback_pump /
  # dm_events / dm_nvs / the behavior shell).
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/dm_render.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/slot_store.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/dm_machine.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/dm_feedback_build.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/dm_feedback_pump.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/behaviors/behavior_dynamic_macro.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST app PRIVATE src/dm_nvs.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS app PRIVATE src/dm_query.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS app PRIVATE src/dm_events.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS app PRIVATE src/events/dynamic_macro_state_changed.c)
endif()
