# TinyVoice v1.5.0 — Public Preview (Beta)

A vocal sound-design box for Ableton Move, as a [Schwung](https://github.com/charlesvestal/schwung) audio FX module (module id `tinyvoice`, library `tinyvoice.so`, archive `TinyVoice.tar.gz`). Sing or talk into the Move and put the voice through one of **four engines**, chosen on the Mode page or from the Remote UI's tabs.

## New in 1.5.0

- **Two new engines.**
  - **Resynth:** hard tuning and harmonic resynthesis. The voice is re-built from its harmonics with a wavetable synth, a filter, widen and warble, tuned from a scale or from the pads. 30 presets ship in a **Default** bank.
  - **Mangler:** a multi-voice pitch and formant shifter. Up to 16 voices with unison and spread, chords from the held pads, a filter, granular or other shift modes. 21 presets ship in its **Default** bank, including choirs and male-to-female / female-to-male voices.
- **Voice FX is now Synth Voice** (same engine, same six presets).
- **A Remote UI** with every control of the active engine at once, live both ways with the Move: open Schwung's manager, go to Modules → TinyVoice and press **Open web UI**.
- **Your own banks** for Resynth and Mangler, managed from the Remote UI: make, rename and delete banks, import presets into one, save the current sound into one, remove a preset. Banks live in `/data/UserData/schwung/tinyvoice-banks/` so reinstalling keeps them, and the Move's **Bank** page only picks. Each engine sees only its own banks.
- **Import presets from the plugins they are inspired by** (Resynth takes `.baby` files, Mangler `.preset` files). Pick or drop your own files into the Remote UI and they are converted in the browser. None of those presets are included in TinyVoice.
- **Looper** (was Freeze): separate Rec and Loop on a Looper page, a 16 s take bar-aligned to the transport, on every engine.
- **A real sung Demo** (a recorded phrase, credited in [CREDITS.md](CREDITS.md)) that tours each engine.
- **On-screen help** for all four engines.

## The four engines

| Engine | What it is |
|---|---|
| **Vocoder** | 13 modes: Robot, Snap, Chords, Prism, Tape, Nightcall, Scream, Dalek, HardTune, Harmony, Walkie, Morph, Double. Five knobs per mode on three pages. |
| **Synth Voice** | A talking filter: your voice through four vowel formants (A E I O U), eight knobs, six presets. |
| **Resynth** | Hard tuning and harmonic resynthesis, with a wavetable synth, filter, widen and warble. Presets page, Bank page, Note mode on the pads. |
| **Mangler** | A multi-voice pitch and formant shifter with chords from the pads. |

There is no built-in delay or reverb: use the chain's own effects after TinyVoice. The [README](README.md) is the short tour; the [guide](docs/guide.md) covers the Vocoder and Synth Voice in full, and [docs/resynth.md](docs/resynth.md) and [docs/mangler.md](docs/mangler.md) the new engines.

## Performance

One allocation when the module is created, none while audio runs. Measured on a Move per 128-frame block (the host allows about 2.3 ms): Synth Voice about 0.12 ms, the vocoder modes 0.1–0.55 ms, Resynth about 0.7 ms, Mangler with 16 voices about 0.56 ms. Output is held under a −6.1 dBFS ceiling. The C builds with `-Wall -Wextra -Werror -pedantic` and the host suite (millions of checks) is clean under UBSan.

## Install

```bash
ssh ableton@move.local "tar -C /data/UserData/schwung/modules/audio_fx -xzf - && chmod 755 /data/UserData/schwung/modules/audio_fx/tinyvoice/tinyvoice.so" < TinyVoice.tar.gz
ssh ableton@move.local "nohup /data/UserData/schwung/restart-move.sh >/dev/null 2>&1 &"
```

Then add **TinyVoice** as an audio FX in a slot, after a sound source such as Line In (the Move's built-in mic works through Line In).

## Known limitations

This is a public preview. It was tuned on synthetic voices, tones and a built-in mic, and checked on a Move with the on-device smoke test, but not judged by ear across many singers. Resynth and Mangler in particular are new and have had the least listening. Headsets, external mics and routing other tracks into TinyVoice are untested. Key and Scale are set on the module's own pages and don't follow Move's project. No licence file yet: all rights reserved until one is added. See the README's [Known limitations](README.md#known-limitations).
