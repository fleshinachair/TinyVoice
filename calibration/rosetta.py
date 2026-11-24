"""Rosetta probe: load every Humanoid preset into the plugin and read every VST parameter back.

Pairs each preset XML id with the VST parameter it drives, and records (xml value, normalised,
display text) so the unit laws can be fitted. Output: calibration/out/rosetta.json
"""
import glob, json, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hum_host as H  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
os.makedirs(OUT, exist_ok=True)

files = sorted(f for f in glob.glob(os.path.join(H.PRESETS, "**", "*.baby"), recursive=True)
               if not f.endswith("wavetables.baby"))
rows = []
for n, f in enumerate(files):
    xml = H.preset_xml(f)
    params = {m.group(1): float(m.group(2)) for m in re.finditer(r'<PARAM id="([^"]+)" value="([^"]*)"', xml)}
    tun = dict(re.findall(r'(tuning_\w+)="([^"]*)"', xml))
    rb = H.param_readback(xml)
    rows.append({"file": os.path.relpath(f, H.PRESETS), "xml": params, "tuning": tun, "vst": rb})
    if n % 20 == 0:
        print(n, len(files), flush=True)
json.dump(rows, open(os.path.join(OUT, "rosetta.json"), "w"), indent=0)
print("wrote", len(rows))
