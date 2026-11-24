# Plan: a Humanoid-style resynthesis personality for TinyVoice

Goal: a third TinyVoice personality that sounds as close as practical to Baby Audio's
Humanoid (v1.2.2, installed on the dev PC), loads Humanoid presets, exposes every Humanoid
control on the Remote UI (`move.local:7700/remote-ui`) and the main ones on the Move.

Status (2026-11-24, end of day 1):
- Phase 0 done: harness, Rosetta map, probes (calibration/FINDINGS.md).
- Phases 1-3 done natively: the engine (hm.c, hmfx.c, fft.c), measured against Humanoid (median 2.7 dB
  1/3-octave distance over 40 presets x 3 real vocals).
- Phase 4 done natively: the Resynth personality in dsp.c, pages, presets importer + deploy script, help,
  tests (1.9 M checks pass), ARM build.
- Phase 5 written: src/ui/web_ui.html (not yet loaded in Schwung).
- Not done: anything on the device (the Move was offline), Phase 6's full 282-preset pass, Phase 7.
- Decisions as recommended: third personality, own name, bring-your-own presets, Duo only, Lo latency
  default (Lo/Mid/Hi are 23/29/52 ms here, lower than Humanoid's 38/85/181 ms).

## 1. What Humanoid is (from the manual, the presets and the plugin itself)

Facts gathered on 2026-11-24:

- **Engine** (manual p.3): "FFT hard tuning and additive resynthesis". The voice is tracked,
  hard-tuned, analysed into harmonics, and resynthesised. A wavetable oscillator can replace
  the voice's harmonics (Transform), with extra synth layers (Note Shift, Octave).
- **The plugin runs headless.** `Humanoid.vst3` loads in DawDreamer (the venv at
  `C:\Github\TinyHouse\.venv`). It renders offline and tunes a 215 Hz harmonic tone to exactly
  220 Hz at INIT. It exposes 62 automatable parameters (plus 128 MIDI-CC dummies).
- **Trial mode** (manual p.15): 5 s of silence every 60 s. The calibration harness must
  render in short pieces on fresh instances and reject any render containing a dropout (section 6).
- **Presets** live in `C:\ProgramData\BABY Audio\presets\Humanoid\`: 180 Factory, 101
  "1 Yr Anniversary" and an INIT preset, 282 in all, plus `wavetables.baby` (59 tables).
  Each `.baby` is XML: `<humanoid_state><Humanoid><PARAM id= value=/>...` (63 params,
  real units), a `<wavetable_tree name size wavetable_base64>` (one single cycle, float32,
  92-4096 samples), and a `<tuning_tree>` (mode, key, scale, duophonic, scale lock, a 12-note
  custom scale mask).
- **Plugin state** is JUCE `VC2!` + XML wrapping the same `humanoid_state`. So any `.baby`
  file can be loaded into the real plugin for A/B measurement.

### Humanoid's controls (VST name → preset id → observed range in the 282 presets)

| Panel | Control | Preset id | Range seen | Notes |
|---|---|---|---|---|
| Pitch | Mode Scale / Note / MIDI, Lock | `tuning_tree` | | Lock = keep tuning across preset loads |
| | Key, Scale, custom notes | `tuning_key/scale/auto_notes` | key 0-11, scale 0-2 | 12-bit mask |
| | Pitch Note Mono / Duo | `pitch_mono/pitch_duo` | MIDI 24-78 | Note mode; Duo = second note |
| | Quantize | `quantize` | 0.03-0.5 | correction speed |
| | Quantize Attack / Hold / Polarity | `tuner_attack/tuner_hold/tuner_polarity` | 0-0.4 / 0-100 / 0-1 | |
| | (unknown) | `relax` | -1, -0.25, 0.1 | not exposed; probe |
| | Scale Bypass | `quantize_enabled`? | 0/1 | probe |
| | Robotify | `robotify` | 0-1 | |
| | Denoise / Metallic / Flatten / Emphasis | `robotify_harm/_res/_flatten/_emphasis` | 0-1 | id↔name to confirm |
| | Formant / Bandwidth | `formant_shift/formant_width` | -1..1 / 0-0.5 | |
| Utility | Range low / high | `min_f0/max_f0` | MIDI 40-60 / 69-84 | |
| | Latency lo / mid / hi | `lookahead` | 0, 1, 2 | analysis frame size |
| | De-ess, Attack, Sensitivity | `deess*` | 0-24 dB | |
| | Gate, Attack, Hold, Release | `noisegate_*` | -80..-20 dB | |
| | Smoothing, Pitch, Timbre | `smoothing*` | 0.1-0.9 | frame-to-frame averaging |
| | Sharpen, Bias | `sharpen/sharpen_bias` | 0-100 / -6..6 | pre-EQ tilt |
| Synthesize | Waveform | `wavetable_tree` | 59 + Saw/Sine/Square/Triangle | per-preset single cycle |
| | Shape, Odd 1-4 / Even 1-3 | `wavetable_shape`, `chebyshev_0..6` | 0-1, -2..2 | Chebyshev waveshaping of the cycle |
| | Stretch | `wavetable_stretch` | 0.1-0.9 | midpoint skew, PWM-like |
| | Transform | `transform` | 0-1 | voice ↔ wavetable morph |
| | Note Shift | `pitch_shift` | -12..12 st | scale-following extra line |
| | Octave | `octaving` | -1..1.5 | octave down/up blend: law to probe |
| | Synth Mix | `synth_mix` | 0-1 | tuned voice ↔ synth section |
| Filter | HPF / LPF cutoff + res, Mid freq / gain / width | `hpf_*`, `lpf_*`, `filter_*` | freqs are MIDI notes (10-138), gains dB | |
| Effects | Widen, Cutoff, Rate | `widen*` | | chorus ensemble |
| | Warble, Rate, Mode LFO/FM | `warble_*` | | vibrato on partials / audio-rate FM |
| | Freeze, Buffer ms / synced | `freeze*` | 1-666 ms, 1/64.. | short glitch loop |
| Output | Input gain, Output gain, Dry/Wet | | ±16 dB, -12.5..+1.3, 0.2-1 | |

## 2. Architecture decision

**A third personality, `engine` = Resynth** (working name; see Decisions). It sits beside
Vocoder and Voice FX in `ui_hierarchy` `modes` and shares the `vt_t`, the input stage
(Freeze, input high-pass, Demo source) and the output stage (headroom, soft limit). Its
params are `hm_*`, so the Vocoder and Voice FX stay bit-identical: the same test pattern
Voice FX uses.

New DSP files (all host-free, as now):

| File | Role |
|---|---|
| `src/dsp/fft.[ch]` | Real FFT, radix-2, sizes 512-4096, tables built in `dsp_init` (the repo has no FFT) |
| `src/dsp/harm.[ch]` | Analysis: STFT, f0 (reusing `cher_mpm` with Range), harmonic peaks with parabolic interpolation, residual noise in ~24 bands, spectral envelope |
| `src/dsp/hmtune.[ch]` | Humanoid tuner: Scale (key + 12-bit mask) / Note (mono, duo) / MIDI (pads, newest two), Quantize, Attack, Hold, Polarity, Smoothing-pitch, Flatten |
| `src/dsp/hmsynth.[ch]` | Wavetable → harmonic spectrum, Shape (Chebyshev), Stretch, Transform morph, Note Shift, Octave, Warble |
| `src/dsp/resyn.[ch]` | Synthesis by inverse FFT (Rodet/Depalle FFT⁻¹ additive): every partial of every layer is written into one spectrum as a window lobe with a continuous phase, plus the shaped residual noise, then IFFT and overlap-add. Cost is fixed per hop whatever the partial count |
| `src/dsp/hmfx.[ch]` | Sharpen, Gate, De-ess, filter (HPF/LPF with resonance, peaking mid), Widen, short Freeze |

**Why a sinusoidal + noise model, not PSOLA or a plain phase vocoder.** Robotify ("force
partials toward perfect multiples"), Denoise ("remove non-harmonic material"), Transform
(per-harmonic morph to a wavetable) and Warble ("vibrato on the partials") only make sense on
a harmonic model. At Robotify 0 the model keeps each partial's measured deviation and the
residual, so it degrades to a phase-vocoder-style tuner, which is what Humanoid sounds like
there.

**Signal path (one hop = 256 samples, 2 Move blocks):**

```
in → Input gain → Sharpen → Gate → STFT (frame 1024/2048/4096 by Latency)
   → f0 (MPM, Range) → tuner target(s) (Scale / Note / MIDI; Duo = 2 targets)
   → harmonics a_k, f_k + residual bands + envelope
   → Robotify: f_k → k·f0 (Metallic), residual × (1-Denoise), f0 wobble removed (Flatten), near-ideal partials boosted (Emphasis)
   → Formant: envelope warped by 2^(shift) over Bandwidth; Smoothing (timbre) across frames
   → Voice layer:  partials at target × pitch ratio
   → Synth layers: wavetable harmonics (Shape, Stretch) × voice loudness, morphed by Transform; + Note Shift line; + Octave line; Warble
   → Synth Mix between the voice and synth layers
   → IFFT + OLA → De-ess → Filter → Widen → Freeze → Output gain → Dry/Wet (dry delayed to latency)
```

**Budget.** Per 128-frame block: about half a 2048-point forward FFT, half a 1024-point
IFFT, MPM per hop (the tracker costs ~0.2 ms today) and partial bookkeeping. Estimate:
0.4-0.8 ms of the 2.3 ms on the Move. **This is the first thing to prove on the device**
(Phase 1 exit gate). Memory: ~150-250 KB more inside `vt_t`, nothing allocated after init.

**Latency.** Humanoid's lo/mid/hi map to analysis frames of about 1024/2048/4096
(≈ 12 / 25 / 50 ms here). The exact values will be measured from the plugin and matched where
the Move allows. Default to Lo for live singing on the Move.

## 3. Presets

**Format on the Move: Schwung User Presets, no file I/O in the DSP.** A preset is
`/data/UserData/schwung/presets/tinyvoice/<bank>/<name>.json` = `{"name","module","version","state"}`.
`state` is TinyVoice's own flat JSON (the format TinyHouse already ships this way). The
wavetable travels inside `state` as one string param `hm_wt`: 128 harmonics × (mag, phase)
as int16, base64, ≈ 700 chars. That makes every preset self-contained and works with the
host's My Presets page, its save/hash, and Remote UI writes (values over 256 bytes take the
64 KB mailbox path, per Schwung `docs/MODULES.md`).

**Bring your own, never shipped.** Humanoid's factory presets and wavetables are Baby
Audio's and its sound designers' work. They are converted from the user's own installation
and not committed or released. The public module ships TinyVoice-original presets and
generic tables (Saw, Square, Triangle, Sine, a few of our own). Two import routes:

1. `scripts/import_humanoid.py` (Python, the dev PC): reads `C:\ProgramData\BABY Audio\presets\Humanoid`,
   maps units with the laws from Phase 0, dedupes wavetables, writes bank folders
   (`Humanoid Factory`, `Humanoid 1 Yr`) and deploys them like TinyHouse's `deploy_presets.sh`
   (staging, checksums, rollback copy).
2. Remote UI **Import .baby** (Phase 5): drop files on the panel; the browser parses the
   XML, sets params live, and can save to the preset store through the manager's `/files/upload`.

`.gitignore` gets `*.baby`, the Humanoid installer and manual, and the generated banks.

## 4. Controls

**Remote UI (`web_ui.html` in the module folder, `schwungRemote` API, keys built from
`schwungRemote.component`, so `fx1:hm_transform` etc.).** Every Humanoid control, laid out by
Humanoid's panels (Pitch, Synthesize, Filter, Utility, Effects, Output), with the
calibration sub-controls inline. Graphics: a harmonic/waveform display of the current table,
the filter curve (draggable), a pitch/target readout fed by a `hm_scope` key, a keyboard for
Note mode and the custom-scale mask. The page drives only the Resynth personality's keys, and
shows a "switch to Resynth" button when another engine is active. All params must be in
`chain_params`; the ones with no Move cell are left out of `ui_hierarchy` (TinyHouse does the
same). The module test that checks `ui_hierarchy` coverage gets an explicit "remote-only" list.

**Move pages (Resynth level):**

| Page | Knobs 1-4 | Knobs 5-8 |
|---|---|---|
| Presets | host browser (`list_param` / `count_param` / `name_param`) | |
| MAIN | Transform, Robotify, Formant, Synth Mix | Note Shift, Octave, Quantize, Dry/Wet |
| PITCH | Pitch Mode, Key, Scale, Q.Attack | Range Lo, Range Hi, Latency, Duo note |
| SYNTH | Wave, Shape, Stretch, Warble | Warble Rate, Warble Mode, Widen, Smoothing |
| TONE | HPF, LPF, Mid Freq, Mid Gain | Gate, De-ess, Sharpen, Output |
| FREEZE | Freeze, Buffer, Sync, Demo | |

Pads: in MIDI mode the newest held pad (two with Duo) sets the target, with the same
`dsp_handle_midi` source filtering as now. Remote-only: Robotify Denoise / Metallic / Flatten
/ Emphasis, Formant Bandwidth, Shape Odd/Even ×7, filter resonances and width, gate, de-ess
and widen times, Sharpen Bias, Smoothing Pitch/Timbre, Polarity, Hold, Input gain.

Risk: `chain_params` grows from ~4.5 KB to ~12 KB. TinyHouse serves a larger one, but this
module's 4.5 KB is itself still unverified on the device (CLAUDE.md open items). Check it in
Phase 0.

## 5. Phases

Each phase ends with the native suite green (`bash scripts/test.sh`), a clean `-Werror
-pedantic` ARM build, and (from Phase 1) `smoke_on_device.sh`.

**Phase 0 — Measure Humanoid (calibration harness).** `calibration/` (Python, DawDreamer from
the TinyHouse venv or its own):
- `hum_host.py`: loads `.baby` presets through the `VC2!` state, renders with dropout
  protection, caches renders by preset hash.
- **Rosetta probe:** load every preset and read back each VST param's normalised value and
  display text, giving exact XML-id ↔ name ↔ unit laws (settles harm/res, `relax`,
  `quantize_enabled`, scale numbering).
- **Single-parameter sweeps from INIT** on fixed inputs: latency per setting, Quantize/Attack
  → retune time, Formant → envelope shift (st), Bandwidth, Robotify sub-controls → harmonicity
  and noise ratio, Octave law, Note Shift scale behaviour, Transform → harmonic profile, Shape
  and Stretch → table spectrum, filter mappings, Widen, Warble rate/mode, Freeze behaviour
  (audio loop or frozen spectrum?), Gate/De-ess/Sharpen curves, output level.
- Output: `calibration/laws.json` + a short report. Also the device checks: `chain_params`
  size, a stub third `engine` mode on the Mode page.

**Phase 1 — Core resynthesis.** FFT, analysis, tuner (Scale/Note/MIDI, Duo), Robotify,
Formant, Smoothing, IFFT synthesis, Latency settings, dry alignment. A native render CLI
(`tools/tv_render.c`: WAV + state → WAV) so the harness can A/B. **Exit gate:** INIT and
tuner-only presets within target on the metrics below; CPU on the Move under 1.0 ms per block.

**Phase 2 — Synthesize.** Wavetable harmonic tables, Shape, Stretch, Transform, Note Shift,
Octave, Synth Mix, Warble.

**Phase 3 — Utility and effects.** Input gain, Sharpen, Gate, De-ess, filter, Widen, Freeze,
Output, Dry/Wet. Headroom stage on (as for the newer modes).

**Phase 4 — Params, Move UI, presets.** `gen_module_json.py`, `k_params`, `dsp.h` in step;
the Resynth level and pages; `hm_wt`; the importer + deploy script; original factory set;
help.json section; tests (bit-identity of the other personalities, every imported preset
finite and under the ceiling, state round trip).

**Phase 5 — Remote UI.** `web_ui.html` (+ `assets/`), Import .baby, packaged by `build.sh`
and `install.sh`; a node test like `canvas_test.mjs` for the parsing/mapping code.

**Phase 6 — Calibration pass over all 282 presets.** For each preset × each test input:
pitch track (cents vs Humanoid), harmonic profile of steady vowels (first 30 harmonics,
dB), ⅓-octave spectrum distance, harmonic-to-noise ratio, loudness, stereo correlation.
The output is a worst-first report and a fixed-input regression set. Iterate on the laws, not
per preset.

### Test material

Synthetic signals mislead (CLAUDE.md says so repeatedly). Use: the Demo phrase, the
existing synthetic vowels, and **real dry vocal takes**: a few 10-20 s mono WAVs (sustained
vowels, a sung phrase, speech, low and high voices), recorded on the PC or the Move.

### Trial-mode dropouts

Render every clip ≤ 20 s on a freshly created plugin instance. Reject a render if the output
holds a run ≥ 0.25 s at < -90 dBFS while the input is active, and retry once on a new
instance. Log retries so a pattern (wall clock vs sample count) shows up.

**Phase 7 — Replace older TinyVoice modes where Resynth is better** (asked 2026-11-24). For each
overlapping mode (Snap, HardTune: hard tuning; Double, Morph; Chords/Prism: harmonies; Robot: synth
voice) make an original Resynth setting (never a Humanoid preset: those cannot ship), A/B it against the
current mode on the vocadito clips and the Demo phrase (spectral distance, tuning error, CPU on the Move,
and by ear), and swap the mode's engine where Resynth wins. Mode names and saved sets stay valid.

## 6. Decisions (recommendation first)

1. **Third personality** rather than replacing Voice FX or adding a 14th vocoder mode.
2. **Name**: an own name on the Move ("Resynth"), with "imports Humanoid presets" in the docs. Not Baby Audio's product name or look.
3. **Presets are bring-your-own** (converted locally, never committed or released).
4. **Duo only** (two targets) to match Humanoid. More pad voices could come later.
5. **Latency default Lo** on the Move, for live singing.

## 7. Model use

- **Opus**: Phase 0 (probe design, reading the measurements), Phase 1 (FFT⁻¹ synthesis,
  analysis, CPU fit), Phase 2, and the Phase 6 tuning. These are judgment-heavy and errors
  there are expensive to find later.
- **Sonnet is fine** for Phase 3's standard effects, Phase 4's plumbing (generator, params,
  importer, deploy script, help.json, tests) and Phase 5's `web_ui.html`. Give it this file,
  `calibration/laws.json` and CLAUDE.md.
