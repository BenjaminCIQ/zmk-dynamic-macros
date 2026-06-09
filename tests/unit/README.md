# Pure-core unit tests (dual-mode harness)

Step 0 of the deep-module rewrite (see `docs/architecture-redesign.md` §4.1, §5).

The pure-core modules (`dm_render`, `slot_store`, `dm_machine`) are Zephyr-free, so
their tests are written **once** and run **two ways**:

1. **Ztest under `west test`** — one CI harness alongside the existing `native_sim`
   snapshot suite.
2. **Standalone host compile** — plain `cc`/`cl`, no Zephyr, for a sub-second local
   red-green loop. This build also doubles as a **decoupling proof**: if a pure module
   stops compiling without Zephyr, the host build breaks immediately.

`ztest_shim.h` brokers the two: under the standalone build (`-DZTEST_SHIM_HOST`) it maps
`zassert_*` onto `assert`/`printf` and supplies test auto-registration; otherwise it
includes the real `<zephyr/ztest.h>`.

## Fast local loop (standalone host)

### Linux / macOS / MinGW (GNU make + gcc/clang)

```sh
cd tests/unit
make            # build + run; prints "OK — N passed, 0 failed"
```

### Windows (MSVC, VS Build Tools)

GNU `make` is not present; invoke `cl` directly inside the VS developer environment:

```bat
"C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64
cd tests\unit
cl /nologo /W3 /DZTEST_SHIM_HOST /I. /I..\..\include host_runner.c test_*.c /Fe:dm_unit_host.exe
.\dm_unit_host.exe
```

The shim auto-registers tests via `__attribute__((constructor))` on GCC/Clang and via the
`.CRT$XCU` section on MSVC, so the same test source runs under all three.

## CI loop (Ztest)

Runs as `native_sim` under Twister via `testcase.yaml`, picked up by the same
`west test` invocation as the snapshot suite.

## Adding tests as modules land

- Each `test_*.c` is one translation unit, compiled by **both** the `Makefile`/`cl` host
  build and `CMakeLists.txt` (Ztest).
- When a pure module is extracted (steps 1/3/4), add its Zephyr-free `.c` next to the test
  that drives it — to `SRCS` in the `Makefile` (host) and it is globbed automatically in
  `CMakeLists.txt` (Ztest sources are `test_*.c`; add module `.c` explicitly there).
- Grow the `zassert_*` surface in `ztest_shim.h` only as a test needs a new macro
  (redesign §6).

`host_runner.c` supplies `main()` + the registry for the host build only; the Ztest build
excludes it (Ztest provides its own `main`).
