"""Tuning probes: pitch modes, scale numbering, the auto-notes mask, Duo, latency per setting."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_lib import *  # noqa

# 12 chromatic steps from C3, each +30 cents sharp, 0.6 s each
STEP = 0.6
notes = [48 + i + 0.30 for i in range(12)]
x = S.voice(S.f0_steps([440 * 2 ** ((m - 69) / 12) for m in notes], STEP), "a", jitter_cents=0)

def run(label, xml):
    y = hum(xml, x)
    outs = [A.midi(out_pitch(y, i * STEP + 0.25, (i + 1) * STEP - 0.05)) for i in range(12)]
    print(f"{label:34s}", " ".join(f"{note_name(m):>7s}" for m in outs))
    return outs

print(f"{'input':34s}", " ".join(f"{note_name(m):>7s}" for m in notes))
for mode in (0, 1):
    run(f"mode {mode} (scale 0, duo 0)", init(tuning_mode=mode, tuning_duophonic=0))

midi_notes = [(60, 100, 0.0, 12 * STEP)]
y = hum(init(tuning_mode=2, tuning_duophonic=0), x, midi_notes)
print(f"{'mode 2 + MIDI C4 held':34s}", " ".join(f"{note_name(A.midi(out_pitch(y, i*STEP+.25, (i+1)*STEP-.05))):>7s}" for i in range(12)))
for sc in (0, 1, 2):
    run(f"mode 0 scale {sc} (mask untouched)", init(tuning_mode=0, tuning_scale=sc, tuning_duophonic=0))
run("mode 0 mask {0,4,7} (C E G)", mask_xml(init(tuning_mode=0, tuning_duophonic=0), {0, 4, 7}))
run("mode 0 key 2 scale 0", init(tuning_mode=0, tuning_key=2, tuning_duophonic=0))
run("Scale Bypass (quantize_enabled 1)", init(tuning_mode=0, tuning_duophonic=0, quantize_enabled=1))
