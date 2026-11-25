# Parameters

> Possibly stale. The generator (`scripts/gen_module_json.py`: VOICE / PITCH / MORE) and `k_params` in `src/dsp/dsp.c` are the truth; check them before trusting a row below.

## Parameters (home page VOICE, then PITCH and MORE)

**Page layout (2026-10-12).** The grid is 4 x 2 (cells 1-4 top, 5-8 bottom) and a page cannot hold a gap. **VOICE**: Mode, Char 1-3... (the five contextual knobs), Mix, Level. **PITCH**: Key, Scale, Retune, Glide, then the scope readouts (cells 5-7). **MORE**: Spread, Tone, Gate, Latch / Beauty, Demo. **LOOPER** (2026-11-25; also Resynth's `rs_looper` page): Rec, Loop, Length under the Freeze banner (they were MORE knobs 3-4 as one Off / Rec / Loop knob). The Parameters table below still lists some knob numbers from earlier layouts: `scripts/gen_module_json.py` (`VOICE`, `PITCH`, `MORE`) is the truth.



Order is `vt_param_id` in `dsp.h`, `k_params` in `dsp.c`, `PARAMS` in `scripts/gen_module_json.py` (the page layout is separate: `VOICE`/`PITCH`/`MORE` in the generator; the root level repeats VOICE's knobs and links the four pages). Enums accept option name or index; floats clamp. Change all three together (the module.json test enforces it, layout included), then regenerate `module.json`.

| Page | Knob | Key | Type / range | Default | Notes |
|---|---|---|---|---|---|
| **VOICE** (home) | 1 | `mode` | Snap, Robot, Chords, Morph, Prism, Double, Tape | Snap | |
| **PITCH** | 1 | `key` | C..B | C | Set by hand: Move doesn't pass its project key to modules. ("Follow" was removed: it silently meant C. A set saved with it loads as C.) Also roots Robot's drone. N/A in Tape (see *Knobs that go inert* below) |
| | 2 | `scale` | Chromatic, Major, Minor, Pentatonic, Dorian | Chromatic | Minor = natural; Pentatonic = major pentatonic. N/A in Robot and Tape |
| | 3 | `retune` | 0-500 ms | 0 | 0 = instant hard snap; Prism ignores it (always 0). N/A in Robot, Prism, Tape |
| | 5-7 | `pitch`, `target`, `cents` | read-only readouts | | Not params: the OLED scope draws across these cells; turning them does nothing |
| **VOICE** | 7 | `mix` | 0-1 | 1 | Wet/dry, dry delay-compensated |
| **VOICE** | 2 | `char1` | 0-1 | 0.5 | Macro 1, per mode; knob named per mode (table below) |
| | 3 | `char2` | 0-1 | 0.5 | Macro 2, per mode |
| | 4 | `spread` | 0-1 | 0.25 | ADT chorus width; Chords also pans its voices by it; in Robot, odd/even band panning instead |
| | 6 | `demo` | Off, On | Off | Back on a knob since Gate took in Auto (2026-10-09). Transient: never saved in or restored from `state` |
| **LOOPER** | 1 | `freeze_rec` (Rec) | Off, Rec | Off | Records a take (see Signal flow). Transient |
| | 2 | `freeze` (Loop) | Off, On | Off | Plays the kept take; never records. Transient |
| | 3 | `freeze_len` (Length) | Free, 1 Bar, 2 Bars, 4 Bars | 2 Bars | Take length in bars at the host tempo. Saved |
| **VOICE** | 8 | `gain` (labelled Level) | -12..+12 dB | 0 | Output, soft-limited |
| | 5 | `tone` | 0-1 | 1 | 2-pole lowpass on the wet signal, 400 Hz-20 kHz, 1 = bypass |
| **PITCH** | 4 | `glide` | 0-500 ms | 0 | Slew between pad notes (lead/harmony targets, Robot carrier). N/A in Tape |
| **VOICE** | 4 | `mod` | 0-1 | 0 | Macro 3, per mode; knob named per mode |
| **MORE** | 1 | `gate` | Auto | Auto, -60 dB .. 0 dB (22 options) | Auto | **Gate and Auto in one knob** (2026-10-09; Auto used to be a switch on its own page that hid this knob). Lowest setting **Auto**: input AGC on every mode (soft voice lifted up to +12 dB toward ~-12 dBFS, loud eased up to -6 dB) plus the -42 dBFS RMS voice gate and expander (see Signal flow), Robot AGC, scale chords with no pads (Chords, Prism); char1/char2/mod stay live as trims over it. Turning clockwise: a manual threshold -60..0 dB in 3 dB steps (21 of them: -60, -57 ... 0), which is Auto Off. An enum so the host shows "Auto" / "-38 dB" itself (a float's value text can't say Auto). Set by option name, by option index (0 = Auto, the host's enum form), or by a negative / fractional dB number, rounded to the nearest 3 dB step (old sets, tests; a saved -38 loads as -39). The old `auto` key is still accepted (On = Auto, Off = the last manual level, -39 dB until one is chosen) and readable; a saved set with `auto` applies it after everything else, so Auto On wins and its dB `gate` is remembered. New sets save `"gate":"Auto"` or `"-39 dB"` and no `auto` |
| | 2 | `latch` | Off, On | Off | Pad chord holds after release in Robot, Chords, Prism, Nightcall, Harmony (see MIDI). Saved. Drawn by `custom:tinyvoice_latch`: On (inverted) / Off in those modes, a dimmed "---" in the others; same cell, still turns |
| **PITCH** | 8 | `beauty` | 0-1 | 0.5 | Per-mode polish (see Engine notes); 0 = the plain engine (in Snap, 0 also makes the retune instant, Retune and Glide aside). Acts in Snap, Robot, Chords, Prism only; N/A in Morph, Double, Tape (see *Knobs that go inert* below) |
| **VOICE** | 5 | `char4` | 0-1 | 0.5 | Macro 4. In Prism, Nightcall, Scream, Dalek, Harmony, Walkie its own value (Refract, Duty, Narrow, Drive, Grain, Squelch); in Robot, Snap, Chords, Tape, HardTune, Morph, Double an **alias** (below). Saved |
| | 6 | `char5` | 0-1 | 0.5 | Macro 5 (Shimmer, Cavity, Line, Width, Micro, Band; HardTune's Sens) or an alias. Saved |

**Knobs that go inert (2026-10-09).** Five knobs do nothing in some modes (`vt_param_acts(id, mode)` in `dsp.h`, read off the engines: `modes.c`, `imogen.c`, `cher.c`, `daft.c`). Everything else (char1/char2/mod, Spread, Gate, Mix, Tone, Delay, Reverb, Gain...) acts in every mode.

| Knob | Acts in | N/A in | Why |
|---|---|---|---|
| `key` | Robot, Snap, Chords, Prism, Nightcall, HardTune, Morph, Double | Tape, Scream, Dalek, Harmony, Walkie | no quantizer there; the vocoders' drone uses the Key |
| `scale` | Snap, Chords, Prism, HardTune, Morph, Double | Robot, Tape, Nightcall, Scream, Dalek, Harmony, Walkie | the drone is root + fifth + octave in any scale |
| `retune` | Snap, Chords, Morph, Double | everything else | Prism is a fixed 0 ms snap, HardTune a fixed instant one; the others have no lead |
| `glide` | Robot, Snap, Chords, Prism, Nightcall, HardTune, Morph, Double | Tape, Scream, Dalek, Harmony, Walkie | no pad targets / carrier |
| `beauty` | Robot, Snap, Chords, Prism | the rest | the engines never read it |

Each is drawn by its own one-cell widget (`custom:tinyvoice_key|scale|retune|glide|beauty`, `extra_keys: ["mode"]`, one `GATED` table in `canvas.js`): the value as the stock knob shows it where it acts (scale names shortened to fit: Chrom / Penta / Dor; ms and % formats), a dimmed "N/A" where it doesn't. Same cell, same knob in every mode (nothing shifts). The DSP **ignores turns** of an inert knob (`dsp_set_param`), so its stored value waits for the next mode that uses it; a `state` / config restore (`restoring` flag) and any write in the first second after load (`warm_samples`, in case the host restores a set key by key) still set it whatever the mode. Tested: `test_inert_knobs` (C) and the `GATED` checks in `canvas_test.mjs` use the same table. Latch has its own widget and "---" (it acts in Robot, Chords, Prism).

**The five contextual knobs** (VOICE knobs 2-6: `char1`, `char2`, `mod`, `char4`, `char5`), named per mode by `k_macro_names` in `dsp.c` (8 characters or fewer, tested). **Each mode has its own five (2026-10-12).** They used to be stored once and merely renamed, so HardTune's Amount at max was Harmony's Lead at max: `p[]` still holds the *running* mode's five (every engine reads them there, unchanged), `macro_store[mode][5]` holds the others, and `macros_follow_mode()` files the running mode's five away and loads the new mode's (the defaults the first time) wherever the mode can change: `set_param("mode")` (any path, a state restore included), and the top of every audio block (the Demo tour steps the mode itself, and restores the user's at its end). The knobs that are aliases of a real param (Retune, Beauty, Glide, Tone) stay shared, because that param is shared. **State**: the running mode's five are `char1`..`char5` as before (so an older set loads: its five land on the running mode, the others at their defaults); the other modes' are written as `"HardTune.char1"`, `"Scream.char4"`... only where they differ from the defaults (a fresh set is 367 bytes, the worst case, every knob of every mode changed, 1.6 KB), and `dsp_set_param` takes those keys (unknown mode or knob names, and non-numbers, are refused; values clamp). Tests: `test_mode_knobs`. `*` = an alias (below).

| Mode | `char1` | `char2` | `mod` | `char4` | `char5` |
|---|---|---|---|---|---|
| Robot | [Shift] formant -12..+12 st (moves the synthesis bank; 0.5 = 0) | [Noise] sibilance noise | [Ensemble] VP-330 ensemble depth | [Beauty]* | [Glide]* |
| Snap | [Formant] formant -12..+12 st on top of a fixed +2.5 st lift (0.5 = +2.5 st) plus pitch coupling (formant follows 25% of the correction at <= 0.5, 100% at 1) and, above 0.5, tighter buzzier grains | [Sens] tracking sensitivity (MPM clarity 0.9..0.4) | [Vibrato] vibrato depth (5.5 Hz, up to 1/2 st) | [Retune]* | [Beauty]* |
| Chords | [Level] harmony level 0..2x vs the lead | [Voicing] voicing (< 0.5 root/3rd/5th/oct down, else wide) | [Humanize] humanize (detune + drift) | [Retune]* | [Beauty]* |
| Prism | [Voicing] harmony spread (close / standard / wide voicing, and pan width) | [Warmth] tape warmth (saturation + darker top) | [Humanize] humanize | [Refract] glass amount 0..1 (0 = the plain engine; mix goes with its square) | [Shimmer] comb feedback 0..0.62 |
| Tape | [Drop] pitch -12..0 st (1.0 = none) | [Flutter] wow & flutter | [Saturate] saturation | [Tone]* | [Hiss] tape hiss, 0 below 0.5 |
| Nightcall | [Formant] synthesis shift -12..+12 st | [Breath] unvoiced bleed 0..35 % (17.5 % at 0.5) | [Rasp] carrier noise 10..30 % (10 % at 0) | [Duty] pulse width 12..28 % (20 % at 0.5) | [Cavity] 1.2 kHz peak 0..+12 dB |
| Scream | [Pitch] -12..-3 st (-8 at 0.5) | [Throat] formant scale 0.84 x 2^(v-.5) | [Grit] soft-clip drive, 0 = clean | [Narrow] band 90 Hz-4.8 kHz at 0.5, closes into a phone line at 1 | [Line] line hiss, 0 below 0.5 |
| Dalek | [Shift] -500..+500 Hz linear (0 at 0.5) | [Ring] ring-mod 0..60 Hz (30 at 0.5; < 1 Hz = off) | [Metal] comb feedback 0..0.7 (3.7 / 4.3 ms) | [Drive] tanh drive 1..5 | [Width] L/R shifts +-0..12 Hz apart |
| HardTune | [Amount] correction depth 0..200 % (100 % at 0.5) | [Formant] -12..+12 st (0 at 0.5) | [Vibrato] 5.5 Hz, up to 1/2 st | [Glide]* | [Sens] tracking sensitivity (as Snap's) |
| Harmony | [Lead] lead level 0..2x | [Stack] taps level 0..2x | [Detune] +-12 ct slow drift per tap | [Grain] 8..18 ms (rounded to whole pitch periods) | [Micro] 0..4.5 ms between taps |
| Walkie | [Peak] presence peak (0..+12 dB, +8 at 0.5) | [Drive] diode clip drive (after the compressor) | [Static] hiss under the voice (faint floor at 0) | [Squelch] receiver burst level (open / close tail) | [Band] peak centre 1.27..2.55 kHz (1.8 kHz at 0.5) |
| Morph | [Throat] formant -12..+12 st | [Drive] drive | [Wobble] formant wobble (0.25 Hz, +-4 st) | [Retune]* | [Glide]* |
| Double | [Detune] double detune 5..30 ct | [Width] stereo width (double level) | [Drift] drift (pitch + Haas time) | [Retune]* | [Glide]* |

**char4 / char5 aliases** (`k_slot_alias`, `slot_alias()` in `dsp.c`): in the older modes the knob is not a new control but the param of that name on a 0..1 scale (`min + v (max - min)`): Robot (Beauty, Glide), Snap and Chords (Retune, Beauty), Tape (Tone, Delay), Morph and Double (Retune, Glide), HardTune (Glide, -). Setting `char4` writes the param; setting the param (its own knob on PITCH / MORE) moves what `char4` reads; one value, two knobs. The aliased param always acts in that mode (`vt_param_acts`, tested), so the inert-knob rule never silences it. A `state` restore (`restoring`) sets only the macro's own storage, so a saved stale `char4` can never overwrite a restored `retune`; `get` returns the alias's value, so in a fresh Snap `char4` reads 0 (Retune 0 ms) and `char5` 0.5 (Beauty).
