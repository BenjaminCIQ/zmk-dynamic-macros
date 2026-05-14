zephyr_include_directories(include)

if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/behavior_dynamic_macro.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST app PRIVATE src/dm_storage.c)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS app PRIVATE src/events/dynamic_macro_state_changed.c)
endif()
