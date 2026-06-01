# ARROW Style Cheat Sheet

Quick reference for the ARROW feedback style. Enable with:

```ini
CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_FEEDBACK_STYLE_ARROW=y
CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACRO_LOCALE_US=y
```

## Grammar

| Char | Role |
|------|------|
| `>` | Success / saved |
| `-` | Deleted |
| `!` | Error prefix |
| `?` | Empty / missing |
| `%` | Full / no capacity |
| `<>` | Move mode |
| `>>` | Move transfer |
| `*` | Record |
| `.` | Stop |

## All Messages

| Event | Output | Example |
|-------|--------|---------|
| Record | `>*` | `>*` |
| Stop | `>.` | `>.` |
| Saved | `>` + slot | `>N0` |
| Saved (verbose) | `>` + slot + `:'preview'` | `>N0:'Hello'` |
| Deleted | `-` + slot | `-N0` |
| Delete failed | `!-` + slot | `!-N0` |
| Save failed | `!>` + slot | `!>N0` |
| Save queue full | `!>%` + slot | `!>%N0` |
| Delete queue full | `!-%` + slot | `!-%N0` |
| Slot empty | `?` + slot | `?N0` |
| Slot full | `>` + slot + `%` | `>N0%` |
| Buffer overflow | `!%` | `!%` |
| Move prompt | `<>` | `<>` |
| Move source selected | `<>` + slot | `<>N0` |
| Move done | `>` + src + `>>` + dst | `>N0>>N1` |
| Move cancel | `<>x` | `<>x` |
| Chain (success) | inline preview only | `Hello` |
| Chain empty | `?` + slot | `?N0` |
| Chain no room | `>>` + slot + `%` | `>>N0%` |
| Feedback level | `>FB:` + level | `>FB:VERBOSE` |
| Status header | `==` + count `/` total + ranges | `==2/8 N0-7 R8-15` |
| Status slot (preview) | `>` + slot `:` + `'preview'(N)` | `>N0:'Hello'(4)` |
| Status slot (no preview) | `>` + slot `:` + count | `>N0:4` |
| Status slot (empty) | `>` + slot `:-` | `>N0:-` |
