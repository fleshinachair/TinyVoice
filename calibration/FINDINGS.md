# Humanoid 1.2.2: what was measured (Phase 0, 2026-11-24)

All numbers are from renders of the real plugin (`hum_host.py`, DawDreamer, 44.1 kHz, 128-sample blocks)
with presets loaded as state. INIT = `00 INIT.baby`. Probe scripts: `probe_*.py`; raw output in
`out/probe_*.txt`. Laws in `laws.json`.

## Harness facts

- **Presets load through the plugin state**: JUCE `VC2!` + `humanoid_state` XML + a private-data tail (`juce_state.py`).
- **The wavetable synth needs the message thread.** Humanoid builds the oscillator through JUCE async
  messages; DawDreamer runs no message loop, so Transform 1 rendered silence until `hum_host.pump()` ran
  the Win32 message loop after load. The voice path never needed it (every other probe was identical).
- **Trial silence** is counted in rendered samples: 60-65 s, 120-125 s... of each instance. Fresh
  instance per render, <= 20 s, so it never appears.
- **A MIDI note in the first ~0.05 s is dropped**; the host starts notes at >= 0.1 s.
- **Range matters, but not as a floor** (re-measured 2026-11-24, Opus pass): with Range Lo at C3 (or C4)
  a steady voice down to D2 is still tracked, in its own octave, at full level, but played ~+47 c sharp
  (E2 -> E2+47, A2 -> A2+49; the same at Range Lo C4 for C3..A3); below ~D2 it breaks. Range Hi has no
  visible effect (E6 tracked exactly with Range Hi A4). Probes below C3 set `min_f0 = 40`.
- **Random silent renders** (not the trial timer): a fresh instance sometimes renders the synth path as
  silence for the whole render (-111 dBFS), up to ~50 % of tries for presets with a reshaped wavetable
  (Shape / Stretch / Chebyshev), presumably the asynchronous wavetable build racing the offline render.
  A warm-up render before the real one made it worse (exact zeros). `probe_lib.hum` re-renders any
  result with a dropout (up to 5 tries) and never caches one that cleared; four presets once listed as
  "silent in Humanoid" (I, Troutman, Rotten Robotics, Dust All Around, Drunken Organist) were this.

## Parameter map (preset id -> VST name -> unit)

Exact pairing from reading back all 282 presets (`rosetta.py`, `out/rosetta.json`). Units are the
preset's: frequencies are MIDI note numbers, gains dB, times seconds.

| preset id | VST parameter | notes |
|---|---|---|
| `pitch_mono`, `pitch_duo` | Pitch Note Mono / Duo | MIDI note |
| `quantize_enabled` | Scale Bypass | 1 = no correction at all (pitch passes) |
| `tuner_attack`, `tuner_hold`, `tuner_polarity` | Quantize Attack / Hold / Invert Polarity | attack 0-0.4 s, hold 0-100 |
| `pitch_shift` | Note Shift | semitones |
| `noisegate_threshold/attack/hold/release` | Gate ... | dB, s |
| `quantize` | Quantize | 0.03-0.5 (norm = 2.27 q - 0.07, clipped) |
| `transform`, `wavetable_shape`, `wavetable_stretch` | Transform, Shape, Stretch | |
| `chebyshev_0..6` | Shape Odd 1, Even 1, Odd 2, Even 2, Odd 3, Even 3, Odd 4 | -2..2 |
| `lpf_freq/res`, `hpf_freq/res`, `filter_freq/gain/width` | filters | freq = MIDI note |
| `lookahead` | Latency lo/mid/hi | 0, 1, 2 |
| `min_f0`, `max_f0` | Range | MIDI note |
| `input_gain`, `output_gain`, `dry_wet` | | dB, dB, 0-1 |
| `synth_mix` | Synthesizer Mix | |
| `formant_shift`, `formant_width` | Formant, Bandwidth | -1..1, 0-0.5 |
| `robotify` | Robotify | |
| **`robotify_res`** | **Robotify Denoise** | (not `_harm`) |
| **`robotify_harm`** | **Robotify Metallic** | |
| `robotify_flatten`, `robotify_emphasis` | Flatten, Emphasis | |
| `octaving` | Octave | -1..1.5 |
| `smoothing`, `smoothing_pitch`, `smoothing_timbre` | Smoothing ... | |
| `deess`, `deess_sensitivity`, `deess_attack` | De-Ess ... | dB, ?, s |
| `sharpen`, `sharpen_bias` | Sharpen, Bias | 0-100, -6..6 |
| `widen`, `widen_cutoff`, `widen_rate` | Widen ... | |
| `freeze`, `freeze_length`, `freeze_sync`, `freeze_synclength` | Freeze ... | ms; sync index |
| `warble_depth`, `warble_rate`, `warble_mode` | Warble ... | mode 0 = FM, 1 = LFO? (see Warble) |
| `relax`, `loosen`, `stickiness` | none | no audible effect found (`relax`) |
| tuning_tree | mode 0 Scale, 1 Note, 2 MIDI; **`tuning_auto_notes` = the 12-note mask actually used**; key/scale are UI only; `tuning_duophonic` = Note mode plays `pitch_duo` too | |

## Behaviour

**Two render paths.** Synth Mix crossfades the *pitch-corrected signal* (path A) with the *resynthesis*
(path B: the harmonic model, Transform, Note Shift, Octave). At 0.5 they sum with partial cancellation
(-4.6 dB, notches), so they are different signals. INIT and most presets are Synth Mix 1 = path B only.
Both paths take Formant and Robotify; Octave and Note Shift exist only in B. Same latency.

**Latency** (gap centres in a steady voice): Lo 24-38 ms, Mid 70-86 ms, Hi ~181 ms.

**Tuner.** Scale mode snaps exactly (0 c on held notes and slow glides) to the nearest note in the mask,
no visible hysteresis. Note mode: everything to `pitch_mono` (+ `pitch_duo` with Duo). MIDI mode: the held
notes (two sound together, Duo flag or not); no note held = silence. Scale Bypass: no correction.

**Quantize = a high-pass on the deviation from the target note.** Static detuning is always removed.
Vibrato passes in proportion to its depth, more when faster: with q <= 0.25 a 40 c vibrato keeps 8 % at
1 Hz, 24 % at 3, 40 % at 5.5, 55 % at 10 Hz (a one-pole high-pass at ~12.6 Hz). Combined with
Flatten x Robotify the higher cutoff wins: cutoff = max(fc_q(norm q), fc_flat(robotify x flatten)),
fc_q: norm 0 -> 4.4 Hz, 0.5 -> 12.6, 0.84 -> 40, 1 -> 275; fc_flat(0.5) = 12.6, fc_flat(1) = infinite.
Polarity, Attack (0-0.4 s) and `relax` showed no effect on these signals (Attack probably acts at
note onsets after silence: not yet probed).

**Resynthesis is transparent** at Robotify 0: every harmonic within +-1 dB of the input, plus a flat
**+2.3 dB** gain.

**Robotify** alone does not change harmonic levels; its sub-controls (scaled by Robotify):
- Denoise: harmonic-to-noise 17.9 dB in -> 22.0 (Robotify 0) -> 25.7 (0.5) -> 32.5 (1). Without
  Denoise 23.8. Path A reaches 27.3 at Robotify 1.
- Metallic: partials snapped to exact multiples (1.0000 at Robotify 1); half-way at 0.5.
- Flatten: see Quantize.
- Emphasis 1 (Robotify 1): upper harmonics +5..+8 dB against H1, about 8 (1 - 1/k) dB.

**Formant**: the envelope moves ~12 st per unit (+-1 = +-1 octave); measured +-2.25 st at +-0.25,
~+-5 at +-0.5, +11 at +1 (the measure under-reads big shifts). Bandwidth changes only large downward
shifts (-1: -1.5 st at width 0, -8 st at 0.5).

**Sharpen** (pre-EQ, scaled by Sharpen/100, at 80): -5.5 dB below ~400 Hz, -1.6 dB at 1 kHz, 0 at 2 kHz,
+2.7 dB at 3 kHz, -0.7 dB above 5 kHz. Bias moves the low shelf: -6 -> +3.8 dB, +6 -> -15 dB.

**Transform**: a dB-domain morph of each harmonic from the voice toward the wavetable, weight about
t^1.7 (e.g. vowel /i/ H4: -27 dB voice, -12 dB saw; -26, -22, -17, -12 at t = .25, .5, .75, 1), at
constant total loudness. At 1 the vowel is gone: /a/ and /i/ give the same saw spectrum.

**Shape / Stretch** reshape the table's harmonics (shape 1: H2 -3 dB, H3 -12): Chebyshev waveshaping
and a phase-distortion knee. **Stretch** (fitted on the saw, 0.1..1.0, within 0.35 dB over 24 harmonics):
up to 0.5 the cycle's midpoint moves to s (knee at (s, 0.5)); above, the cycle is squeezed into the first
1 - s and nearly held after (knee at (1 - s, 1 - 2 (1 - s)^2)); it saturates at 0.9 (0.9, 0.95, 1.0
identical). A symmetric skew was 5-9 dB too dark at 0.9-1. (Replacing the INIT table's base64 with a
sine or triangle changed nothing: Humanoid rebuilds its named "Saw".)

**Synth Mix is a linear crossfade**: with the two paths on different pitches (Note Shift +7) path A
is x (1 - sm) and B x sm within 0.1 dB from 0.1 to 0.9. On the same pitch they interfere: -4.8 dB at
0.5 on an A3 vowel, -4.4 on E3, varying per harmonic (-1 .. -23 dB) and with the pitch and Transform.

**Note Shift transposes path B** (it is not an added line): at Synth Mix 1 only the shifted voice is
heard (the original pitch -85 dB), at 0.5 the voice and the shifted line at -6 dB each, at 0 the voice
alone. In Scale mode the shifted note snaps into the mask (A3 +7 -> E4, +4 -> C4). The Octave lines
follow the shifted pitch. A "Fifth Above" harmony therefore needs Synth Mix around 0.5.

**Loudness follows the voice, not the line's pitch**: Note mode from -7 to +24 st away from the sung
note keeps the output within ~1 dB (a line read at its own harmonics loses 6-14 dB going up).

**Octave**: negative = a sub-octave line (-1: ~-4 dB vs the main, -0.5: ~-10 dB), positive = an
octave-up line (+1.5: even harmonics +7.5 dB). Path B only. Where its partials coincide with the main
line's (Transform 1, octave -1: main -2.7 dB, sub +3 dB vs ours) the two interfere: not modelled.

**Filters** (a saw at 82 Hz, Transform 1, harmonics vs no filter): HPF / LPF edges are brick walls
(-80 dB 0.08 octave past the cutoff); a negative resonance is a roll-off from ~1.4 octaves inside the
passband at ~0.85 |res| dB per octave (res -40: -10 dB an octave in, -27 at 0.6; -80: -50 at 0.6);
a positive one a peak of about res dB 0.15 octave inside the edge (11..24 dB within 1-2 dB). The mid
band is a Gaussian in log frequency, sigma = 0.01645 x Width octaves (8..60 within 1 %), gain as set.

**Output**: Out Gain is exact from -24 to +9 dB and stops there (+12 gives +9.0). In Gain is exact
(-16..+16). A **peak limiter** holds the output near -0.5 dBFS: a +19 dB resonance on the fundamental
came out with peaks at -0.7 dBFS and everything else 8 dB down; below the ceiling it does nothing.

**Warble**: LFO mode rate 0.5 -> 4.7 Hz, 1 -> 10 Hz, ~+-1.3 st at depth 1; the other mode is faster
(audio-rate FM; the tracker reads 24-40 Hz). Rate 0 = none.

**Widen**: a chorus; L/R correlation 0.87 at 0.5, 0.44 at 1 (cutoff 0.8-1 hardly matters, rate 0.3 -> 0.53).

**Level**: INIT output = input + 2.3 dB (RMS), constant across Robotify, Transform and the vowel.

## Not yet probed

Attack at onsets, Hold, Smoothing (timbre/pitch), De-ess, Gate, Freeze, Warble mode naming, unvoiced
input (consonants, speech), Duo levels, the output limiter's release.
