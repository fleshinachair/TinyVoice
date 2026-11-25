# Toolchain and scripts

## Toolchain

Zig is the C compiler. **Target is glibc, not musl**: Move's host `dlopen`s the module into a glibc 2.35 process, and a musl `.so` would drag a second libc in.

```
zig cc -target aarch64-linux-gnu.2.35 -O2 -shared -fPIC -fvisibility=hidden \
    -std=c11 -Wall -Wextra -Werror -pedantic \
    -Isrc/host -Isrc/dsp \
    src/dsp/dsp.c src/dsp/cher.c src/dsp/daft.c src/dsp/imogen.c src/dsp/psola.c src/dsp/modes.c \
    src/dsp/freeze.c src/dsp/demo.c src/dsp/voice.c src/dsp/plugin.c \
    -o build/tinyvoice/tinyvoice.so -lm
```

- Never add `-ffast-math`: it can pull in `libmvec`, which the device lacks. `build.sh` fails if `libmvec` appears (when `nm` exists).
- Output is `tinyvoice.so` (see ABI section), mode 755. Package layout is `tinyvoice/{tinyvoice.so,module.json,canvas.js,help.json}`, archived as `build/TinyVoice.tar.gz` (the archive is named for the product, the folder inside is the module id: the host needs folder == id). The build is warning-free under `-Wall -Wextra -Werror -pedantic` (it no longer needs `-Wno-unused-parameter`).
- Env overrides: `ZIG`, `TARGET`, `OPT`.
- Windows: zig installed via winget (`zig.zig`, 0.17.0). New shells see it on PATH; an already-open Git Bash session may need `PATH="$PATH:<winget zig dir>"`. Run the scripts with `bash scripts/...`.
- Verified: the exported symbol is present, no `libmvec`, and the highest glibc symbol version required is 2.34 (`pthread_create`, for the preset loader; it was 2.27). On the device the loader resolves only `libm.so.6` and `libc.so.6`. `nm` is not installed on this machine, so `build.sh`'s symbol check is skipped here; check by hand (`strings`/Python) or install binutils.

## Scripts

| Command | What it does |
|---|---|
| `bash scripts/test.sh` | Builds `tests/test_main.c` + engine natively (`zig cc`, falls back to `cc`/`gcc`/`clang`; `CC=` overrides) with `-DVT_TEST_ALLOC`, runs from the repo root. `SANITIZE=1` adds ASan+UBSan (Linux/macOS toolchains; with zig on Windows, `-fsanitize=undefined` alone works and the full suite is clean under it) |
| `bash scripts/build.sh` | Fetches host headers if missing, cross-compiles, copies `module.json` + `ui.js`, writes `build/tinyvoice/` and `build/TinyVoice.tar.gz` |
| `bash scripts/install.sh [--build]` | `tar | ssh` sideload to `/data/UserData/schwung/modules/audio_fx/tinyvoice` as `ableton@move.local` (`MOVE_HOST`, `MOVE_USER`, `DEST`); removes stale `dsp.so`/`ui.js`, sets 755 on `*.so` |
| `bash scripts/smoke_on_device.sh [path]` | Cross-compiles `tests/device_smoke.c` and runs it on the Move against the installed `.so`: dlopen, entry symbol, config JSON, MIDI via the exported symbol, all 13 modes (audible + per-block time), `state` restore. Proves the library is good independent of the UI |
| `bash scripts/fetch_host_headers.sh` | Downloads the two Schwung headers into `src/host/` |

The test runner exits non-zero on any failure and checks for leaks via counting allocators (`vt_test_calloc/free`; `dsp.c` swaps in under `VT_TEST_ALLOC`). It also parses `module.json` and checks it against the C param table: key order, types, ranges, options, defaults, `ui_hierarchy` coverage, and setter/getter clamping. Edit `module.json` and the `k_params` table in `dsp.c` together, or that test fails.
