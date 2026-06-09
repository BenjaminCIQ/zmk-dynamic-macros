if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  zephyr_include_directories(include)

  if (CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_NEW_STACK)
    # Deep-module rewrite stack (test-only during the rewrite; the parity harness
    # selects it). The pure core (dm_render/slot_store/dm_machine/dm_feedback_build/
    # dm_query) plus the Zephyr shells (dm_feedback_pump/dm_events/dm_nvs/the v2
    # behavior shell). DM_NEW_STACK selects the new query projection and the v2
    # shell, so the legacy symbols are not compiled and cannot collide.
    zephyr_compile_definitions(DM_NEW_STACK)
    target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/dm_render.c)
    target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/slot_store.c)
    target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/dm_machine.c)
    target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/dm_feedback_build.c)
    target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/dm_feedback_pump.c)
    target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/behaviors/behavior_dynamic_macro_v2.c)
    target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST app PRIVATE src/dm_nvs.c)
    target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS app PRIVATE src/dm_query.c)
    target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS app PRIVATE src/dm_events.c)
    target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS app PRIVATE src/events/dynamic_macro_state_changed.c)
  else()
    # Legacy single-path (the live/shipping path, untouched until the step-8 cut-over).
    target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/behaviors/behavior_dynamic_macro.c)
    target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO app PRIVATE src/dm_feedback.c)
    target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_PERSIST app PRIVATE src/dm_storage.c)
    target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_EVENTS app PRIVATE src/events/dynamic_macro_state_changed.c)
  endif()
endif()
