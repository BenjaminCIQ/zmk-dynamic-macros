# Firmware-size measurement

Self-contained scaffolding to measure the dynamic-macro module's flash/RAM cost
across its config combinations, on a real **nice!nano v2** target, including a
**baseline with the module entirely absent** so the README's "Firmware Size"
section can show both the floor and each config's delta.

Driven by `.github/workflows/firmware-size.yml` (manual trigger — Actions tab →
"Firmware size" → Run workflow). Not a per-push gate; footprint changes rarely.

## What it builds

A 6-entry matrix, all the same board + shield, differing only in config:

| id | Config | Module |
| --- | --- | --- |
| 0-baseline | (none) | **absent** — behavior node omitted via `-DDM_SIZE_BASELINE` |
| 1-minimal | `minimal.conf` | RAM only, no feedback, no events |
| 2-persist-nofeedback | `persist_nofeedback.conf` | NVS, no feedback |
| 3-feedback-nopersist | `feedback_nopersist.conf` | verbose feedback, RAM only |
| 4-default | `default.conf` | shipping defaults (feedback + NVS) |
| 5-full | `full.conf` | feedback + events + NVS + auto-erase |

## How the baseline stays truly module-free

The module auto-enables when its devicetree node (`zmk,behavior-dynamic-macro`,
from `behaviors/dynamic_macro.dtsi`) is present — `dt_compat_enabled` in `Kconfig`.
`dm_size.keymap` includes that node only when `DM_SIZE_BASELINE` is *not* defined.
The baseline build passes `-DEXTRA_DTC_FLAGS=-DDM_SIZE_BASELINE`, so the node is
absent, the auto-enable never fires, and none of the module compiles. The other
builds get the node + a binding from every command family.

## Reading the result

The `report` job posts a markdown table to the run's **Step Summary** (and uploads
it as the `firmware-size-table` artifact) with flash/RAM per config and Δ vs.
baseline. `flash = text + data`, `ram = data + bss`, from `arm-none-eabi-size -B`
on `zephyr.elf`. Paste the numbers into README.md's Firmware Size table.

## Files

- `west.yml` — workspace manifest (ZMK + this module as `self`)
- `boards/shields/dm_size/` — minimal shield (mock kscan, the switchable keymap,
  BLE/display off so the delta is the module's code, not unrelated subsystems)
- `*.conf` — one config fragment per matrix row

## Note

This rail is **CI-validated only** — it requires a west/Zephyr/ARM toolchain not
present on the maintainer's local box, so its first real check is the workflow run.
If the first run needs tuning, the usual suspects are devicetree-flag propagation
(`EXTRA_DTC_FLAGS` → the keymap preprocessor) and shield discovery (`BOARD_ROOT`).
