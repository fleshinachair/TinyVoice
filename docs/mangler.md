# Mangler (the fourth engine)

`src/dsp/mangle.[ch]`: a multi-voice pitch / formant shifter modelled on Devious Machines' PitchMonster. The laws (pitch and formant maths, unison and spread, time spread, filter, mix, level) were measured against the VST in the calibration lab (`C:/Github/TinyVoice-lab/pitchmonster/FINDINGS.md`, outside this repo). Judge by ear on a real voice: the lab only used synthetic signals and two vocals.

## Engine

Three cores share one 131072-sample input ring and the tracker's period (`cher`, run every sample in `dsp.c`):

- **Granular** per voice, a two-tap rotating-delay shifter; each tap restarts at Grain Rate Hz whatever the shift, Hann-ish windows half a period apart; Jitter gives each grain its own length. Formants move with the pitch.
- **Formant** per voice, TD-PSOLA grains (2 periods) on pitch marks, emitted at period / r and read at the formant ratio (Fix On: formants stay; Fix Off: they follow the shift). Unvoiced input passes at ratio 1.
- **Vocode** Formant's core with the output pitch an absolute note: C4 + Pitch + the note offset, whatever is sung.

Voices = note offsets x Unison, at most 16 (unison per note is reduced to fit). Pitch Spread puts a note's unison voices evenly over [-S/2, S/2] (Scatter: random per grain), Time Spread delays them over 0..T (Smear: random per grain); each voice x 1/sqrt(N); Wide pans them. Then a 2nd-order high-pass and low-pass (Filter: Freq +/- Width/2 octaves), PitchMonster's Mix law (dry = min(1, 2(1-w)), wet = min(1, 2w)) and Volume. Latency is ours, not PitchMonster's 30 ms: a few samples (Granular) to about half a period plus the grain reach (Formant, Vocode).

## In the module

- `engine` option **Mangler** (`VT_ENGINE_MANGLER`, `VT_MODE_MANGLER`). Runs the automatic front end (input AGC and voice gate, threshold = Resynth's Mic Gate `hm_mic_gate`), the -6.1 dBFS headroom stage, and its own Mix / Volume (the vocoder's Mix, Level, Tone and Spread are out of the way).
- Params `mg_*` (`VT_P_MG_FIRST .. VT_P_MG_LAST`, in PitchMonster's units): `mg_shift` Granular/Formant/Vocode, `mg_pitch` -48..24 st, `mg_formant` -24..24 st, `mg_voices` 1..8, `mg_detune` 0..12 st, `mg_time` 0..1000 ms, `mg_grain` 1..100 Hz, `mg_mix`, `mg_notes` Normal/Pads/Chord, `mg_chord` 1..8, `mg_mono`, `mg_fix`, `mg_snap`, `mg_scatter`, `mg_smear`, `mg_wide`, `mg_gjitter`, `mg_filter`, `mg_freq`, `mg_width`, `mg_volume`.
- The eight **chord slots** are not params: `"mg_chords":"60,64,67;62,65,69;..."` in `state` (up to 8 notes per slot; notes are offsets from C4), read and written by `dsp.c` (`mg_chords_parse` / `_format`), also as a get / set key `mg_chords` (the Remote UI chord fields). Eight plain chords are the defaults (`mg_default_chords`).
- **Pads**: the held pads (or the latched chord, or the Demo's chord) oldest first, moved by whole octaves until their mean is in C4's neighbourhood (`mg_pad_list`: Move's pads sit high). No pad held = silent.
- **Looper** (Rec / Loop / Length) works as on the other engines: it sits ahead of the engine, on the input.
- **Presets**: the same Presets / Bank pages as Resynth (`rs_preset`, `rs_bank`), listing the Mangler bank while the engine is Mangler (`rs_bank_of` in `dsp.c`). `rsbank_load_engine("mangler", ...)` keeps only states that run Mangler: **Default** = `src/presets/Mg - *.json` (ours, 14), **Imported** = Schwung User Presets that run Mangler (not split by kind). Choosing one resets Mangler's controls and chord slots to defaults, then applies the state. Demo steps through the first six Default presets, 4 bars each, and puts your sound back.
- **Move pages** (generator: `MG_MAIN`, `MG_MORE`, `MG_FILTER`, `LOOPER`): Main (Shifter, Pitch, Formant, Voices, Detune, Time, Grain, Mix), More (Notes, Chord, Mono, Fix, Snap, Scatter, Smear, Wide), Filter (Filter, Freq, Width, Jitter, Volume, Mic Gate, Latch, Demo), Looper, Presets, Bank. The Latch widget shows "---" unless Notes = Pads.
- **Remote UI**: a Mangler tab (Shift, Voices, Notes with the eight chord fields as note names, Filter / Output), controls that do nothing dim from `MG_ACTS`.

## PitchMonster presets (yours, not ours)

`python scripts/import_pitchmonster.py` reads your PitchMonster install's `*.preset` files (XML: real-unit `PARAM`s and `Chord_n` slots) and writes Schwung User Presets to `build/pitchmonster_presets/` (gitignored). `bash scripts/deploy_pitchmonster_presets.sh` copies them to the Move's TinyVoice preset folder; they appear in Mangler's Imported bank. Never commit or ship them (`.gitignore` also covers `*.preset`).

## Tests and cost

`tests/mangler_tests.inc` (pitch of each core, chord / pads notes, state round trip, isolation from the other engines, extremes under the headroom ceiling, CPU, the engine-filtered bank, Demo); fixtures in `tests/fixtures/mg_default`. The device smoke test (`scripts/smoke_on_device.sh`) loads every Default and Imported preset and times the heaviest setting (16 voices Formant): about 0.54 ms per 128-frame block on the Move (the block is 2.9 ms).

## Not done / ideas

Saving the Looper's take as a WAV (a Save control); learning a chord from the held pads; the open sound differences from PitchMonster (Whispers in the Dark's deep notch near 5 kHz, Apocalyptic's highs with 8 voices + Wide + time jitter, Vocode level): see the lab's `pitchmonster/FINDINGS.md`.
