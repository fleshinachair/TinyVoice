# Credits

**Demo voice.** The sung phrase the Demo plays (`src/demo/demo_voice.raw`, shipped in the module as `demo_voice.raw`)
is a 7-second excerpt of recording 25 from the **vocadito** dataset, trimmed, faded and level-matched.

- Rachel Bittner, Katherine Pasalo, Juan José Bosch, Gabriel Meseguer Brocal and David Rubinstein, *vocadito: A dataset of solo vocals with f0, note, and lyric Annotations*, Zenodo, 2021 (v3), DOI 10.5281/zenodo.5578807.
- Source: https://zenodo.org/records/5578807
- Licence: Creative Commons Attribution 4.0 International (CC BY 4.0), https://creativecommons.org/licenses/by/4.0/ . Changes: excerpted, faded in and out, resampled to 44.1 kHz mono, gain-adjusted.

`scripts/make_demo_voice.py` cuts it; give it another recording or start time to change the clip.

**Resynth's Default presets** (`src/presets/`) and their wavetables are TinyVoice's own. Baby Audio Humanoid presets are not
included in this repository or in the module: they are converted on your own machine from your own copy (see
`scripts/import_humanoid.py`).
