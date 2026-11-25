# Schwung ABI, device notes, open items

## Schwung ABI (`move_audio_fx_init_v2`)

Entry symbol exported from `tinyvoice.so`: `audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host)`.

- Struct `audio_fx_api_v2_t`: `api_version=2`, `create_instance(module_dir, config_json)`, `destroy_instance`, `process_block(inst, int16_t *audio_inout, frames)`, `set_param(inst, key, val)`, `get_param(inst, key, buf, len)` (returns bytes written or -1), `on_midi(inst, msg, len, source)`.
- Audio: **stereo interleaved int16, in place**, 44100 Hz, 128-frame blocks (`dsp_process_block` chunks anything up to any size via `VT_MAX_BLOCK=512` stack buffer).
- Instance-based: Schwung may create several; no globals except the stored `host` pointer in `plugin.c`.
- **Realtime contract**: every entry point runs on the SPI audio callback (~2.3 ms slack per block). After `dsp_init` there is no allocation, locking, I/O, or large stack use. All buffers are fixed-size members of `vt_t`.
- **MIDI needs a second exported symbol**: `void move_audio_fx_on_midi(void *inst, const uint8_t *msg, int len, int source)`. The chain host `dlsym`s it to decide whether an FX gets MIDI at all; the struct's `on_midi` alone is not enough (pads were silently never delivered until this was exported). Every MIDI-handling FX on the device exports it (`ducker`, `capicola`, `magneto`, `trance-gate`); `module.json` flags are not what enables it. `build.sh` and `smoke_on_device.sh` both check for it.
- `on_midi` in the struct should still be non-NULL.
- `get_param` keys beyond the params: `state` (JSON snapshot), `viz` (UI feed), `scope` (widget feed), `diag` (live levels for checking a real mic: input 10 ms RMS max and floor over ~1 s, after the input high-pass, voice gate and Robot gate open share, Robot AGC / leveller, output RMS max, physical pads, latch buffer, running mode), `voices`, `name`, `chain_params` and `ui_hierarchy` (served from `module_meta.h`, chain_params with the current mode's macro names), `is_loading` (see Screen), and the read-only readouts `pitch`, `target` (Hz) and `cents`.
- `state` is a flat JSON object `{"mode":"Snap","key":"C",...}` (every param except Freeze and Demo). The loader applies recognised members, ignores unknown keys and nested values, and rejects non-objects. `create_instance` applies `config_json` through the same path.
- `viz` (text): pitch modes `c,<f0_hz>,<target_hz>,<cents>`; Robot `d,<b0>..<b15>` (0–255 on a 60 dB scale).
- **The library must be named `<id>.so`** and `module.json`'s `dsp` must match. The chain module builds `…/modules/audio_fx/<id>/<id>.so` (verified from its strings and from every working FX on the device: `gate.so`, `tapedelay.so`, …). A file called `dsp.so` makes the module appear in the browser but fail to load. (The upstream docs say `dsp.so`; the device is authoritative.)
- Host headers: `plugin_api_v1.h`, `audio_fx_api_v2.h` from `charlesvestal/schwung` (`src/host/`). Pin with `SCHWUNG_REF`.

## Device notes (Move, BusyBox + glibc 2.35)

- Shell tools are BusyBox: no `ldd`/`curl`/`journalctl`, `head -N` is invalid (use `head -n N`). Check linking with `LD_TRACE_LOADED_OBJECTS=1 /lib/ld-linux-aarch64.so.1 <so>`.
- Logs are in `/data/UserData/schwung/` (`debug.log`, `schwung.log`, `schwung-manager.log`); FX load failures are not obviously logged there. Use `smoke_on_device.sh` to separate library faults from metadata/path faults.
- **Reading a live instance**: `bin/schwung-testd` (Schwung's test daemon, not running by default) listens on 127.0.0.1:47777 with a line protocol: `SLOT 3` (0-based), `GET_PARAM fx1:<key>`, `SET_PARAM fx1:<key> <value>`, `STATE` (includes `speaker_active`, `line_in_connected`), `INJECT_MIDI <8 hex>` (a USB-MIDI packet as hardware: presses a real pad). BusyBox `nc` has no `-w`: `(printf 'SLOT 3
GET_PARAM fx1:diag
'; sleep 1) | nc 127.0.0.1 47777`. Start it, query, kill it.
- **Measured on the device (2026-10-09, built-in mic via Line In, trim 0, speakers off)**: room with nobody singing, 10 ms RMS floor -45..-54 dBFS, peaks -29..-40 dBFS each second, so the default Gate (-38) opens 5-35% of the time on the room alone, and Robot (leveller up to 5.6x) played it at up to -9 dBFS. A realistic voice through Robot computes identically on the device and natively (same numbers from a cross-compiled probe).
- Debug switches are empty flag files in `/data/UserData/schwung/`, read at process (or chain) start: `debug_log_on` (verbose `debug.log`, grows fast), `chain_debug_on`, `chain_midi_trace_on` (logs `chain_trace IN <status> <d1> <d2>` per slot to `chain_debug.log`). Remove them when done.
- Display is 128×64. `clear_screen`, `print`, `fill_rect`, `host_module_get_param/set_param` exist for UI scripts (see other modules' `ui_chain.js`).

## Open items / unverified

- **Resynth is installed and smoke-tested on the Move (2026-11-24; 282 imported presets in My Presets) but unheard and unseen**: the five pages, the Remote UI panel (web_ui.html: never loaded in Schwung's manager yet), the help pages' pixel widths (`tests/host/test_help_content_width.sh`), and the imported presets through My Presets. Then judge by ear against Humanoid on a real voice.

- **Voice FX and the presets are unheard and unseen on the device** (built and tested natively, UBSan clean, `-Werror -pedantic` ARM build): the Mode page, the Presets browser and the one-page layout through `modes` / `mode_param`; whether the host's `get_param` buffer takes the served `chain_params` (now ~4.5 KB, was 3.5 KB; check that the widgets still mount); Voice FX's sound on a real voice (formant table, makeup, drive curve are tuned on synthetic tones and the Demo). `scripts/install.sh` now stages and `mv`s files in place instead of overwriting the loaded `.so`.

- **2026-10-11 voicing pass (Scream, Walkie, Prism glass, Demo breath; delay/reverb/feedback removed)**: tuned against the built-in Demo phrase and synthetic tones only; judge by ear on the device. Measured on the Demo, Prism's steady-tone envelope modulation (3-15 Hz) went 8.3 % -> 4.9 % (Refract 0 = 4.3 %). The Demo phrase now ends in a ~0.2 s breath so the voice gate closes once a phrase (Walkie's squelch tail, Robot's silent pauses are audible in the tour). `scripts/` and the tests carry no delay / reverb / feedback; a saved set that names them ignores those keys.

- **v1.0.0 on a Move (2026-10-11)**: installed (`tinyvoice.so` hash equal to the build's), Schwung restarted with no errors in the logs, `scripts/smoke_on_device.sh` passes for all 13 modes (timings in the CPU note above). **Not yet done: hearing the new modes** (Nightcall's carrier/EQ/bleed, Scream, Dalek, HardTune, Harmony, Walkie were designed and measured on synthetic signals only) **and seeing the UI** (below).
- The VOICE / PITCH / SPACE / MORE layout, the five per-mode knob names (`@4@` / `@5@` placeholders, re-read on the `is_loading` pulse), the `custom:tinyvoice_*` widgets' new positions (the scope on PITCH 5-7, Latch on MORE 6, the N/A cells on PITCH) and whether the host honours a hierarchy whose root repeats a first level named VOICE are unseen on the device.
- Decisions taken without asking (the brief was to work independently), easy to reverse: Morph and Double are kept after the roster (drop them from `k_mode_opts` / `MODES` for exactly eleven); Robot and Snap keep following pads; Scream is PSOLA, not WSOLA; the headroom ceiling is applied to the six new modes only; the single 8-knob page is the home level, the other three pages stay (Freeze, Latch, Key, Scale, Gate... would otherwise have no knob).
- Dalek's wet is ring-modulated at the default (30 Hz) so the default sounds like a Dalek rather than like the dry voice: with Ring 0 and Shift 0.5 it is the dry voice through the Hilbert pair.

- The OLED widget is confirmed on the device; Demo's on-screen mode stepping is not yet confirmed.
- Not yet seen on the device: the VOICE/PITCH/MORE layout, the read-only readout cells under the scope, the per-mode knob names via the `is_loading` pulse (and whether a reload while Demo steps modes keeps you on your page), Freeze/Demo as knobs.
- Pad MIDI is confirmed reaching the slot (chain trace: note-ons `0x93` for Slot 4, i.e. the slot's channel, notes 76/77/79). The octave fold that makes them audible in Snap is not yet heard on the device.
- The per-param `default` in `chain_params` is accepted by other modules' inline params but unconfirmed for `chain_params`.
- The format of the host's `config_json` passed to `create_instance` is assumed to be flat `{"key": value}`.
- Delay time is in ms only; no tempo sync (host offers `get_bpm`).
- **Move's project Key/Scale do not reach modules** (re-checked on the device): `host_api_v1` has `get_bpm` but no key/scale call; the shim knows `mRootNote`/`mScale` only as members of Move's model it reflects, and nothing in the shadow UI or chain host sends a key to a module. So Key/Scale are set on TinyVoice's PITCH page, and the Key knob offers only the 12 roots (the old "Follow" option and its `host_root` hook are gone). If a future Schwung passes the project key, add it back as a real option. Reading the set file ourselves is out: no I/O on the audio thread.
- Snap's phase-reset grit and the coupling amounts are tuned on synthetic signals only; judge by ear on a real voice (`cut`/`entry` in `psola.c`, `couple` and the 50 ms fade in `modes.c`).
- Real-voice tuning still to do for every mode (all verification so far is on synthetic signals): Robot makeup and sibilance, tracker clarity, PSOLA level vs ratio, Morph/Prism/Tape drive amounts.
