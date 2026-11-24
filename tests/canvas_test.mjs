// Runs src/ui/canvas.js (the OLED widget) against a mock frame, the way
// Schwung's widget_registry drawCustom hands it one. Needs node; run by
// scripts/test.sh when node is available.
//
//   node tests/canvas_test.mjs
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import vm from "node:vm";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
vm.runInThisContext(readFileSync(join(root, "src/ui/canvas.js"), "utf8"), { filename: "canvas.js" });

let failures = 0, checks = 0;
const check = (cond, what) => { checks++; if (!cond) { failures++; console.log("  FAIL", what); } };

const W = 128, H = 15;
function mockCtx() {
    const px = new Uint8Array(W * H);
    const texts = [];
    let outOfBounds = 0;
    return {
        width: W, height: H, px, texts,
        get outOfBounds() { return outOfBounds; },
        fillRect(x, y, w, h, c) {
            if (x < 0 || y < 0 || x + w > W || y + h > H || w < 0 || h < 0) outOfBounds++;
            for (let j = Math.max(0, y); j < Math.min(H, y + h); j++)
                for (let i = Math.max(0, x); i < Math.min(W, x + w); i++) px[j * W + i] = c ? 1 : 0;
        },
        print(x, y, s, c) { texts.push(String(s)); },
        textWidth(s) { return String(s).length * 6; },
    };
}
const lit = (ctx) => ctx.px.reduce((a, b) => a + b, 0);
const litRow = (ctx, y) => { let n = 0; for (let i = 0; i < W; i++) n += ctx.px[y * W + i]; return n; };
const litCol = (ctx, x, y0, y1) => { let n = 0; for (let j = y0; j <= y1; j++) n += ctx.px[j * W + x]; return n; };

const ov = globalThis.canvas_overlay;
check(ov && ov.widgetKinds && typeof ov.widgetKinds["custom:tinyvoice_scope"].draw === "function", "registers custom:tinyvoice_scope");
const draw = ov.widgetKinds["custom:tinyvoice_scope"].draw;
const T = globalThis.TINYVOICE_SCOPE_FOR_TEST;
const scope = (mode, frozen, phase, loopS, f0, tgt, cents, bands) =>
    ["S", mode, frozen, phase, loopS, f0, tgt, cents, ...(bands || new Array(16).fill(0))].join(",");
const run = (values, nowMs) => { const ctx = mockCtx(); draw(ctx, { values, nowMs, group: { keys: ["retune"] } }); return ctx; };

// note names
check(T.noteName(440) === "A4" && T.noteName(261.63) === "C4" && T.noteName(0) === "--", "note names");

// pitch view: target note, needle off-centre for +20 cents
let c = run({ scope: scope(1, 0, 0, 0, 445, 440, 20) }, 1000);
c = run({ scope: scope(1, 0, 0, 0, 445, 440, 20) }, 2000);       // let the needle settle
check(c.texts.includes("A4"), "pitch: target note shown");
check(c.outOfBounds === 0, "pitch: stays inside the frame");
const mid = Math.round(((18 + 4) + (W - 1)) / 2);
let right = 0, left = 0;
for (let x = mid + 3; x < W; x++) right += litCol(c, x, 0, 3);
for (let x = 22; x < mid - 2; x++) left += litCol(c, x, 0, 3);
check(right > 0 && left === 0, "pitch: sharp voice puts the needle right of centre");

// unvoiced: "--", no needle
c = run({ scope: scope(1, 0, 0, 0, 0, 0, 0) }, 3000);
check(c.texts.includes("--"), "pitch: unvoiced shows --");

// robot bars: a peak in bands 7-8, nothing drawn outside the frame
const bands = new Array(16).fill(0); bands[7] = 255; bands[8] = 200;
c = run({ scope: scope(0, 0, 0, 0, 0, 0, 0, bands) }, 4000);
check(c.outOfBounds === 0, "bars: inside the frame");
const step = Math.floor(W / 16);
check(litCol(c, 7 * step + 1, 0, H - 1) === H, "bars: band 7 full height");
check(litCol(c, 2 * step + 1, 0, H - 1) === 1, "bars: quiet band shows only its floor tick");

// Nightcall is a vocoder too: the same bars (mode 5); a pitch mode (Scream, 6) is not
c = run({ scope: scope(5, 0, 0, 0, 0, 0, 0, bands) }, 4050);
check(litCol(c, 7 * step + 1, 0, H - 1) === H, "bars: Nightcall draws the analysis bands too");
c = run({ scope: scope(6, 0, 0, 0, 220, 220, 0, bands) }, 4060);
check(c.texts.includes("A3"), "pitch: Scream (a tracker mode) draws the pitch view, not bars");

// bars fall smoothly instead of vanishing
c = run({ scope: scope(0, 0, 0, 0, 0, 0, 0, new Array(16).fill(0)) }, 4100);
const h7 = litCol(c, 7 * step + 1, 0, H - 1);
check(h7 > 1 && h7 < H, "bars: a silenced band decays over time, not instantly");

// freeze: dotted loop line plus a playhead that moves on between reads
const fz = scope(1, 1, 0.25, 2.0, 220, 220, 0);
let c1 = run({ scope: fz }, 10000);
let c2 = run({ scope: fz }, 10500);                                // same reading, 0.5 s later
const head = (ctx) => { for (let x = W - 1; x >= 0; x--) if (ctx.px[(H - 3) * W + x]) return x; return -1; };
check(litRow(c1, H - 1) > W / 4, "freeze: loop line drawn");
const moved = head(c2) - head(c1);
check(Math.abs(moved - Math.round(0.25 * (W - 1))) <= 2, `freeze: playhead runs on by itself (moved ${moved}px)`);
check(c1.outOfBounds === 0 && c2.outOfBounds === 0, "freeze: inside the frame");

// recording: a bar that fills with the buffer, growing on between reads
const rec = scope(1, 2, 0.25, 4.0, 220, 220, 0);                  // a quarter full after 4 s
c1 = run({ scope: rec }, 20250);                                   // (times with the blinking head off)
c2 = run({ scope: rec }, 21250);                                   // 1 s later, no new read
const recEnd = (ctx) => { for (let x = W - 1; x >= 0; x--) if (ctx.px[(H - 1) * W + x]) return x; return -1; };
check(Math.abs(recEnd(c1) - Math.round(0.25 * (W - 1))) <= 1, `rec: bar at the fill (${recEnd(c1)}px)`);
check(Math.abs(recEnd(c2) - Math.round(0.3125 * (W - 1))) <= 2, `rec: bar grows on by itself (${recEnd(c2)}px)`);
const c3 = run({ scope: rec }, 22000);                              // later, head on (stale: holds the fill)
check(recEnd(c3) >= recEnd(c1), "rec: blinking head at the end of the bar");
check(c1.outOfBounds === 0 && c2.outOfBounds === 0, "rec: inside the frame");

// freeze tags top right: ARM (blinking, while waiting for the downbeat), REC, LOOP
c = run({ scope: scope(1, 3, 0, 0, 220, 220, 0) }, 30000);         // 30000/250 = 120: even, shown
check(c.texts.includes("ARM"), "freeze tag: ARM while armed");
c = run({ scope: scope(1, 3, 0, 0, 220, 220, 0) }, 30250);         // odd: blinked off
check(!c.texts.includes("ARM"), "freeze tag: ARM blinks");
c = run({ scope: scope(1, 2, 0.25, 4.0, 220, 220, 0) }, 31000);
check(c.texts.includes("REC") && !c.texts.includes("ARM"), "freeze tag: REC while recording");
c = run({ scope: scope(0, 1, 0.5, 2.0, 0, 0, 0, new Array(16).fill(100)) }, 32000);
check(c.texts.includes("LOOP"), "freeze tag: LOOP while looping (Robot bars too)");
check(c.outOfBounds === 0, "freeze tag: inside the frame");

// ---- Freeze banner (MORE knobs 3-4) ----
check(ov.widgetKinds["custom:tinyvoice_freeze"] && typeof ov.widgetKinds["custom:tinyvoice_freeze"].draw === "function",
      "registers custom:tinyvoice_freeze");
const drawFz = ov.widgetKinds["custom:tinyvoice_freeze"].draw;
const fzScope = (frozen, phase, loopS, beat) =>
    ["S", 0, frozen, phase, loopS, 220, 220, 0, ...new Array(16).fill(0), 0, -1, 0, beat].join(",");
/* a two-cell frame that records where every string lands (6 px glyphs, 8 px tall) */
function fzCtx(Wf) {
    const Hf = 15, px = new Uint8Array(Wf * Hf), runs = [];
    let oob = 0;
    return {
        width: Wf, height: Hf, px, runs,
        get outOfBounds() { return oob; },
        fillRect(x, y, w, h, c) {
            if (x < 0 || y < 0 || x + w > Wf || y + h > Hf) oob++;
            for (let jj = Math.max(0, y); jj < Math.min(Hf, y + h); jj++)
                for (let ii = Math.max(0, x); ii < Math.min(Wf, x + w); ii++) px[jj * Wf + ii] = c ? 1 : 0;
        },
        print(x, y, t, c) { const w = String(t).length * 6; runs.push({ x, y, w, t: String(t) }); if (x < 0 || x + w > Wf || y < 0 || y + 8 > Hf) oob++; },
        textWidth(t) { return String(t).length * 6; },
    };
}
const runFz = (values, nowMs, Wf = 64) => { const k = fzCtx(Wf); drawFz(k, { values, nowMs, group: { keys: ["freeze", "freeze_len"] } }); return k; };
const overlaps = (k) => { for (let a = 0; a < k.runs.length; a++) for (let b = a + 1; b < k.runs.length; b++) {
    const A = k.runs[a], B = k.runs[b];
    if (Math.abs(A.y - B.y) < 8 && A.x < B.x + B.w && B.x < A.x + A.w) return true; } return false; };
const texts = (k) => k.runs.map((r) => r.t);
const litFz = (k) => k.px.reduce((a2, b2) => a2 + b2, 0);

// OFF: just the two values, each centred in its own cell - no banner, no status strings
let fk = runFz({ scope: fzScope(0, 0, 0, 5.2), freeze: "Off", freeze_len: "2 Bars" }, 40000);
check(texts(fk).join("|") === "Off|2 Bars", `banner off: only the two values (${texts(fk).join("|")})`);
check(fk.runs[0].x + fk.runs[0].w <= 32, "banner off: the Freeze value inside its own cell");
check(fk.runs[1].x + fk.runs[1].w <= 64 && fk.runs[1].x > fk.runs[0].x + fk.runs[0].w, "banner off: the length to its right, whole");
check(!overlaps(fk) && fk.outOfBounds === 0, "banner off: no collisions, inside the frame");
fk = runFz({ scope: fzScope(0, 0, 0, -1), freeze: 0, freeze_len: 3 }, 40100);       // index forms
check(texts(fk).join("|") === "Off|4 Bars", "banner off: index values read as names");

// ARMED: cleared and taken over, flashing; the beats left to bar 1 as squares
fk = runFz({ scope: fzScope(3, 0, 0, 5.2), freeze_len: "2 Bars" }, 41000);         // 41000/125 even: inverted
check(texts(fk).length === 1 && /ARMED/.test(texts(fk)[0]), `banner armed: one ARMED line (${texts(fk)[0]})`);
check(litFz(fk) > 64 * 15 / 2, "banner armed: inverted");
const sqLit = (k) => { let n = 0; for (let x = 0; x < 64; x++) n += k.px[(15 - 3) * 64 + x] === 0 ? 1 : 0; return n; };
check(sqLit(fk) === 3 * 4, `banner armed: 3 beats left drawn as 3 squares (${sqLit(fk) / 4})`);
const fkOff = runFz({ scope: fzScope(3, 0, 0, 5.2), freeze_len: "2 Bars" }, 41125);
check(/ARMED/.test(texts(fkOff)[0] || "") && litFz(fkOff) < 64 * 15 / 2, "banner armed: flashes");
check(texts(runFz({ scope: fzScope(3, 0, 0, 5.2) }, 41000, 120))[0] === "[ *** ARMED *** ]", "banner armed: full brackets when they fit");

// REC and LOOP
fk = runFz({ scope: fzScope(2, 0.5, 2.0, 9.0), freeze_len: "2 Bars" }, 42000);
check(texts(fk).length === 1 && /REC/.test(texts(fk)[0]) && litFz(fk) > 64 * 15 / 2, `banner rec: inverted REC (${texts(fk)[0]})`);
fk = runFz({ scope: fzScope(1, 0.25, 4.0, 12.0), freeze_len: "2 Bars" }, 43000);
check(texts(fk).length === 1 && /LOOP/.test(texts(fk)[0]), `banner loop: one LOOP line (${texts(fk)[0]})`);

// every state, narrow or wide: in the frame, no collisions, text clear of the bottom rows
let clean = true;
for (const Wf of [50, 60, 64, 120])
    for (const stt of [0, 1, 2, 3])
        for (const tm of [44000, 44125]) {
            const k = runFz({ scope: fzScope(stt, 0.9, 4.0, 3.9), freeze: "Rec", freeze_len: "4 Bars" }, tm, Wf);
            if (k.outOfBounds || overlaps(k) || k.runs.some((r) => r.y + 8 > 15 - 2)) clean = false;
        }
check(clean, "banner: every state fits 50-120 px frames without collisions");

// not frozen: no loop line
c = run({ scope: scope(1, 0, 0, 0, 220, 220, 0) }, 11000);
check(litRow(c, H - 1) === 0 || litRow(c, H - 1) < 4, "live: no loop line");

// junk in: no throw, keeps the last good picture's state
let threw = false;
try {
    run({ scope: "garbage" }, 12000);
    run({ scope: "S,1,2,3" }, 12001);
    run({ scope: undefined }, 12002);
    run({}, 12003);
    draw(mockCtx(), undefined);
} catch (e) { threw = true; console.log("  threw:", e); }
check(!threw, "junk input never throws");

// Auto gain, pad and note-on indicators (the three optional trailing fields)
const scope2 = (agc, pad, ons) => scope(1, 0, 0, 0, 220, 220, 0) + `,${agc},${pad},${ons}`;
c = run({ scope: scope2(18.2, -1, 5) }, 20000);
check(c.texts.includes("+18"), "auto: input boost shown as +18");
check(c.outOfBounds === 0, "auto: inside the frame");
c = run({ scope: scope2(0.4, -1, 5) }, 20100);
check(!c.texts.some((t) => t.startsWith("+")), "auto: no readout when not boosting");

c = run({ scope: scope2(0, 64, 6) }, 20200);                          // a pad (E4) arrived
check(c.texts.includes("E4"), "pad: its note is the label");
check(c.px[0 * W + 0] === 1, "pad: label drawn inverted (lit box behind it)");
check(c.px[(H - 2) * W + (W - 1)] === 1, "pad: note-on flashes the activity dot");
c = run({ scope: scope2(0, 64, 6) }, 21000);                          // no new note-on since
check(c.px[(H - 2) * W + (W - 1)] === 0, "pad: the dot goes out again");

c = run({ scope: scope(0, 0, 0, 0, 0, 0, 0, new Array(16).fill(255)) + ",12,-1,6" }, 22000);
check(c.texts.includes("+12") && c.outOfBounds === 0, "auto readout over the bars, inside the frame");

// ---- Latch (MORE knob 6): On / Off where it acts, a dimmed "---" elsewhere ----
check(ov.widgetKinds["custom:tinyvoice_latch"] && typeof ov.widgetKinds["custom:tinyvoice_latch"].draw === "function",
      "registers custom:tinyvoice_latch");
const drawLt = ov.widgetKinds["custom:tinyvoice_latch"].draw;
function ltCtx(Wl) {
    const Hl = 15, px = new Uint8Array(Wl * Hl), runs = [];
    let oob = 0, cleared = 0;
    return {
        width: Wl, height: Hl, px, runs,
        get outOfBounds() { return oob; }, get cleared() { return cleared; },
        fillRect(x, y, w, h, c) {
            if (x < 0 || y < 0 || x + w > Wl || y + h > Hl) oob++;
            for (let jj = Math.max(0, y); jj < Math.min(Hl, y + h); jj++)
                for (let ii = Math.max(0, x); ii < Math.min(Wl, x + w); ii++) px[jj * Wl + ii] = c ? 1 : 0;
        },
        setPixel(x, y, c) { if (x < 0 || y < 0 || x >= Wl || y >= Hl) oob++; else { px[y * Wl + x] = c ? 1 : 0; if (!c) cleared++; } },
        print(x, y, t, c) { const w = String(t).length * 6; runs.push({ x, y, w, t: String(t), c }); if (x < 0 || x + w > Wl || y < 0 || y + 8 > Hl) oob++; },
        textWidth(t) { return String(t).length * 6; },
    };
}
const runLt = (values, Wl = 32) => { const k = ltCtx(Wl); drawLt(k, { values, nowMs: 0, group: { keys: ["latch"] } }); return k; };
const MODES = ["Robot", "Snap", "Chords", "Prism", "Tape", "Nightcall", "Scream", "Dalek", "HardTune", "Harmony", "Walkie", "Morph", "Double"];
const ACTS = ["Robot", "Chords", "Prism", "Nightcall", "Harmony"];
let ltOk = true;
for (const Wl of [24, 32, 40])
    for (let mi = 0; mi < MODES.length; mi++)
        for (const latch of ["Off", "On", 0, 1]) {
            for (const modeVal of [MODES[mi], mi, String(mi)]) {             // names and index forms
                const k = runLt({ mode: modeVal, latch }, Wl);
                const on = latch === "On" || latch === 1;
                const want = ACTS.includes(MODES[mi]) ? (on ? "On" : "Off") : "---";
                const one = k.runs.length === 1 && k.runs[0].t === want;
                const dim = want !== "---" || k.cleared > 0;                  // "---" is dimmed
                const inverted = want !== "On" || (k.runs[0] && k.runs[0].c === 0 && k.px.reduce((a, b) => a + b, 0) > 0);
                const centred = k.runs[0] && Math.abs((k.runs[0].x + k.runs[0].w / 2) - Wl / 2) <= 3;
                const below = k.runs.every((r) => r.y + 8 <= 15 - 2);         // clear of the bottom rows
                if (!(one && dim && inverted && centred && below && k.outOfBounds === 0)) {
                    ltOk = false;
                    console.log("  latch widget", Wl, MODES[mi], JSON.stringify(modeVal), latch, JSON.stringify(k.runs), k.cleared, k.outOfBounds);
                }
            }
        }
check(ltOk, "latch: every mode x value x width shows On/Off (acts) or a dimmed --- (doesn't), centred, in the frame");
const same = (a, b) => JSON.stringify(a.runs.map((r) => [r.x, r.y])) === JSON.stringify(b.runs.map((r) => [r.x, r.y]));
check(same(runLt({ mode: "Robot", latch: "Off" }), runLt({ mode: "Chords", latch: "Off" })) &&
      same(runLt({ mode: "Snap", latch: "On" }), runLt({ mode: "Tape", latch: "Off" })), "latch: same place in every mode (nothing shifts)");
check(runLt({ latch: "On" }).runs[0].t === "On" && runLt({ mode: "junk", latch: "Off" }).runs[0].t === "Off",
      "latch: mode not read yet shows the value");
threw = false;
try { drawLt(ltCtx(32), undefined); runLt({}); runLt({ mode: null, latch: "maybe" }); drawLt({ width: 32, height: 15, print() {}, fillRect() {} }, { values: { mode: "Snap" } }); }
catch (e) { threw = true; console.log("  threw:", e); }
check(!threw, "latch: junk input never throws (no setPixel on the frame either)");

// ---- Knobs that do nothing in some modes: Key, Scale, Retune, Glide, Beauty ----
// Where a knob acts it shows its value; where it doesn't, a dimmed "N/A". The expected
// table below is the same one test_main.c checks against vt_param_acts in dsp.h.
const EXPECT = {
    key:    { acts: ["Robot", "Snap", "Chords", "Prism", "Nightcall", "HardTune", "Morph", "Double"],
              vals: [["C", "C"], ["F#", "F#"], [2, "D"], ["11", "B"]] },
    scale:  { acts: ["Snap", "Chords", "Prism", "HardTune", "Morph", "Double"],
              vals: [["Chromatic", "Chr"], ["Major", "Maj"], [2, "Min"], ["Pentatonic", "Pen"], [4, "Dor"]] },
    retune: { acts: ["Snap", "Chords", "Morph", "Double"], vals: [["0", "0"], ["120", "120"], [500, "500"]] },
    glide:  { acts: ["Robot", "Snap", "Chords", "Prism", "Nightcall", "HardTune", "Morph", "Double"], vals: [["0", "0"], ["37.4", "37"], ["500", "500"]] },
    beauty: { acts: ["Robot", "Snap", "Chords", "Prism"], vals: [["0", "0%"], ["0.45", "45%"], [0.5, "50%"], ["1", "100%"]] },
};
let gtOk = true;
for (const [key, ex] of Object.entries(EXPECT)) {
    const kind = "custom:tinyvoice_" + key;
    check(ov.widgetKinds[kind] && typeof ov.widgetKinds[kind].draw === "function", "registers " + kind);
    const drawG = ov.widgetKinds[kind].draw;
    const runG = (values, Wl = 32) => { const k = ltCtx(Wl); drawG(k, { values, nowMs: 0, group: { keys: [key] } }); return k; };
    for (const Wl of [24, 32, 40])
        for (let mi = 0; mi < MODES.length; mi++)
            for (const [raw, want] of ex.vals)
                for (const modeVal of [MODES[mi], mi, String(mi)]) {
                    const k = runG({ mode: modeVal, [key]: raw }, Wl);
                    const acts = ex.acts.includes(MODES[mi]);
                    const r = k.runs[0];
                    const one = k.runs.length === 1 && r && (acts ? (r.t.startsWith(want) || want.startsWith(r.t)) && r.t.length > 0 : r.t === "N/A");
                    const fits = r && r.w <= Wl - 2;                                    // in the cell with a pixel to spare
                    const dim = acts ? k.cleared === 0 : k.cleared > 0;                 // "N/A" is dimmed, a value is not
                    const centred = r && Math.abs((r.x + r.w / 2) - Wl / 2) <= 3;
                    const below = k.runs.every((q) => q.y + 8 <= 15 - 2);
                    if (!(one && fits && dim && centred && below && k.outOfBounds === 0)) {
                        gtOk = false;
                        console.log("  " + key + " widget", Wl, MODES[mi], JSON.stringify(modeVal), raw, JSON.stringify(k.runs), k.cleared, k.outOfBounds);
                    }
                }
    // the same cell centre and row in every mode: nothing shifts
    const ctr = (k) => [k.runs[0].x + k.runs[0].w / 2, k.runs[0].y];
    const near = (a2, b2) => Math.abs(ctr(a2)[0] - ctr(b2)[0]) <= 3 && ctr(a2)[1] === ctr(b2)[1];
    const v0 = ex.vals[0][0], v1 = ex.vals[ex.vals.length - 1][0];
    check(MODES.every((m) => near(runG({ mode: m, [key]: v0 }), runG({ mode: MODES[0], [key]: v1 }))), key + ": same cell centre and row in every mode (nothing shifts)");
    check(runG({ [key]: v0 }).runs[0].t.startsWith(ex.vals[0][1]) && runG({ mode: "junk", [key]: v0 }).runs[0].t.startsWith(ex.vals[0][1]),
          key + ": mode not read yet shows the value");
    threw = false;
    try { drawG(ltCtx(32), undefined); runG({}); runG({ mode: null, [key]: "x" }); runG({ mode: "Tape", [key]: 7 }); runG({ mode: "Snap", [key]: null });
          drawG({ width: 32, height: 15, print() {}, fillRect() {} }, { values: { mode: "Tape" } }); }
    catch (e) { threw = true; console.log("  threw:", e); }
    check(!threw, key + ": junk input never throws (no setPixel on the frame either)");
}
check(gtOk, "gated knobs: every mode x value x width shows the value (acts) or a dimmed N/A, centred, fitted, in the frame");
check(Object.entries(EXPECT).every(([k2, ex]) => JSON.stringify(T.GATED[k2].acts) === JSON.stringify(ex.acts)),
      "gated knobs: canvas.js acts lists equal the expected table");

// ---- Resynth: knobs that do nothing in the current settings (custom:tinyvoice_rsna, RS_ACTS)
{
    const kind = ov.widgetKinds["custom:tinyvoice_rsna"];
    check(kind && typeof kind.draw === "function", "registers custom:tinyvoice_rsna");
    const R = T.RS_ACTS;
    const acts = (k, v) => R[k]((key) => v[key]);
    // the rules, read off hm.c / hmfx.c / dsp.c
    const base = { hm_synth_mix: 1, hm_transform: 1, hm_tuning_mode: "Scale", hm_midi_idle: "Tune", hm_quantize_enabled: "Off",
                   hm_pitch_shift: 0, hm_tuning_duophonic: "Off", hm_warble_depth: 0, hm_warble_rate: 0.5, hm_filter_gain: 0,
                   hm_autolevel: "On", hm_freeze_sync: "Off", hm_wavetable_shape: 0, hm_noisegate_threshold: -80, hm_deess: 0,
                   hm_widen: 0, hm_sharpen: 80 };
    const w = (o) => Object.assign({}, base, o);
    const cases = [
        ["hm_transform", {}, true], ["hm_transform", { hm_synth_mix: 0 }, false],
        ["hm_pitch_shift", { hm_synth_mix: 0 }, false], ["hm_octaving", { hm_synth_mix: 0.4 }, true],
        ["hm_wave", {}, true], ["hm_wave", { hm_transform: 0 }, false], ["hm_wavetable_shape", { hm_synth_mix: 0 }, false],
        ["hm_wavetable_stretch", { hm_transform: "0" }, false], ["hm_chebyshev", { hm_wavetable_shape: 0.5 }, true], ["hm_chebyshev", {}, false],
        ["hm_key", {}, true], ["hm_key", { hm_tuning_mode: "Note" }, false], ["hm_scale", { hm_tuning_mode: "MIDI" }, true],
        ["hm_scale", { hm_tuning_mode: "MIDI", hm_midi_idle: "Silent" }, false], ["hm_key", { hm_quantize_enabled: "On" }, false],
        ["hm_key", { hm_quantize_enabled: "On", hm_pitch_shift: 7 }, true], ["hm_key", { hm_quantize_enabled: "On", hm_pitch_shift: 7, hm_synth_mix: 0 }, false],
        ["hm_key", { hm_tuning_mode: 1 }, false], ["hm_key", { hm_tuning_mode: "2", hm_midi_idle: 1 }, false],
        ["hm_tuning_duophonic", {}, false], ["hm_tuning_duophonic", { hm_tuning_mode: "Note" }, true],
        ["hm_pitch_mono", { hm_tuning_mode: "Note" }, true], ["hm_pitch_duo", { hm_tuning_mode: "Note" }, false],
        ["hm_pitch_duo", { hm_tuning_mode: "Note", hm_tuning_duophonic: "On" }, true],
        ["hm_midi_idle", {}, false], ["hm_midi_idle", { hm_tuning_mode: "MIDI" }, true], ["latch", { hm_tuning_mode: "MIDI" }, true], ["latch", {}, false],
        ["hm_warble_rate", {}, false], ["hm_warble_rate", { hm_warble_depth: 0.3 }, true],
        ["hm_warble_mode", { hm_warble_depth: 0.3 }, true], ["hm_warble_mode", { hm_warble_depth: 0.3, hm_warble_rate: -0.5 }, false],
        ["hm_filter_freq", {}, false], ["hm_filter_freq", { hm_filter_gain: -6 }, true],
        ["hm_output_gain", {}, false], ["hm_output_gain", { hm_autolevel: "Off" }, true], ["hm_output_gain", { hm_autolevel: 0 }, true],
        ["hm_freeze_synclength", {}, false], ["hm_freeze_synclength", { hm_freeze_sync: "On" }, true], ["hm_freeze_length", {}, true],
        ["hm_noisegate_attack", {}, false], ["hm_noisegate_release", { hm_noisegate_threshold: -40 }, true],
        ["hm_deess_attack", { hm_deess: 6 }, true], ["hm_widen_rate", {}, false], ["hm_sharpen_bias", {}, true], ["hm_sharpen_bias", { hm_sharpen: 0 }, false],
    ];
    let bad = 0;
    for (const [k, o, want] of cases) if (acts(k, w(o)) !== want) { bad++; console.log("    rule", k, JSON.stringify(o), "want", want); }
    check(bad === 0, "rsna: every rule as read off the engine (" + cases.length + " cases)");
    check(acts("hm_wave", {}) === null && acts("hm_key", { hm_tuning_mode: "Scale" }) === null && acts("hm_output_gain", {}) === null,
          "rsna: unread keys leave the rule undecided");

    // drawn: the value as the stock knob shows it, or a dimmed N/A
    const runR = (key, values, Wl = 32, metaIndex) => { const k = ltCtx(Wl); kind.draw(k, { values, nowMs: 0, group: { kind: "custom:tinyvoice_rsna", keys: [key] }, metaIndex }); return k; };
    let k = runR("hm_wavetable_shape", w({ hm_wavetable_shape: 0.25 }));
    check(k.runs.length === 1 && k.runs[0].t === "0.25" && k.cleared === 0, "rsna: an acting knob shows its value (0.25), not dimmed");
    k = runR("hm_wavetable_shape", w({ hm_wavetable_shape: 0.25, hm_transform: 0 }));
    check(k.runs.length === 1 && k.runs[0].t === "N/A" && k.cleared > 0, "rsna: Shape at Transform 0 is a dimmed N/A");
    check(runR("hm_output_gain", w({ hm_output_gain: -6, hm_autolevel: "Off" }), 64).runs[0].t === "-6.0 dB", "rsna: dB as the host formats it");
    check(runR("hm_output_gain", w({ hm_output_gain: -6, hm_autolevel: "Off" }), 40).runs[0].t === "-6.0dB" &&
          runR("hm_output_gain", w({ hm_output_gain: -6, hm_autolevel: "Off" }), 32).runs[0].t === "-6.0", "rsna: a narrow cell drops the space, then the unit");
    check(runR("hm_pitch_shift", w({ hm_pitch_shift: 7 }), 64).runs[0].t === "+7 st", "rsna: semitones as the host formats them");
    check(runR("hm_wave", w({ hm_wave: "Square" }), 64).runs[0].t === "Square" && runR("hm_wave", w({ hm_wave: 3 })).runs[0].t === "Sine", "rsna: enums by name or index");
    check(runR("hm_pitch_mono", w({ hm_pitch_mono: 60, hm_tuning_mode: "Note" })).runs[0].t === "C4", "rsna: Note 1 as a note name");
    check(runR("hm_filter_freq", w({ hm_filter_freq: 98.29, hm_filter_gain: 3 })).runs[0].t === "98.3", "rsna: step 0.5 gives one decimal");
    check(runR("hm_freeze_synclength", w({ hm_freeze_synclength: "1/8", hm_freeze_sync: "On" })).runs[0].t === "1/8", "rsna: Glitch division");
    check(runR("hm_wave", { hm_wave: "Saw" }).runs[0].t === "Saw", "rsna: before the rule's keys are read, the value shows");
    const mi = { getOrGuess: (key) => ({ key, type: "float", step: 0.1, unit: "dB" }) };
    check(runR("hm_transform", w({ hm_transform: 0.5 }), 64, mi).runs[0].t === "0.5 dB", "rsna: the host's metaIndex is used for the text when given");
    let threw = false;
    try {
        kind.draw(ltCtx(32), undefined); runR("nope", {}); runR("hm_wave", { hm_wave: null, hm_transform: "x" }); runR("hm_key", w({ hm_tuning_mode: "??" }));
        kind.draw({ width: 32, height: 15, print() {}, fillRect() {} }, { values: w({ hm_transform: 0 }), group: { keys: ["hm_wave"] } });
        runR("hm_wave", w({}), 32, { getOrGuess() { throw new Error("x"); } });
    } catch (e) { threw = true; console.log("  threw:", e); }
    check(!threw, "rsna: junk input never throws");
    let fit = true;
    for (const Wl of [20, 24, 32, 40]) for (const key of Object.keys(T.RS_META)) {
        const kk = runR(key, w({ [key]: 0 }), Wl);
        fit = fit && kk.outOfBounds === 0 && kk.runs.length === 1;
    }
    check(fit, "rsna: every knob at every cell width stays in the frame");

    // Latch: in Resynth it acts in MIDI mode, whatever the vocoder's Mode is
    check(runLt({ engine: "Resynth", mode: "Snap", hm_tuning_mode: "MIDI", latch: "On" }).runs[0].t === "On", "latch: Resynth MIDI mode shows the value");
    check(runLt({ engine: "Resynth", mode: "Robot", hm_tuning_mode: "Scale", latch: "On" }).runs[0].t === "---", "latch: Resynth Scale mode is ---");

    // module.json: every gated knob carries the widget with the keys its rule reads; the formats match
    const mj = JSON.parse(readFileSync(join(root, "module.json"), "utf8"));
    const cp = {}; for (const p of mj.capabilities.chain_params) cp[p.key] = p;
    let vizOk = true;
    for (const [key, deps] of Object.entries(T.RS_DEPS)) {
        const v = cp[key] && cp[key].viz;
        if (!v || v.kind !== "custom:tinyvoice_rsna" || JSON.stringify(v.extra_keys) !== JSON.stringify(deps)) { vizOk = false; console.log("    viz", key, JSON.stringify(v)); }
        const seen = new Set(); R[key]((x) => { seen.add(x); return base[x] !== undefined ? base[x] : 1; });
        for (const x of seen) if (deps.indexOf(x) < 0) { vizOk = false; console.log("    rule", key, "reads", x, "not in extra_keys"); }
        const m = T.RS_META[key], c2 = cp[key];
        if (m.type === "enum" && JSON.stringify(m.options) !== JSON.stringify(c2.options)) { vizOk = false; console.log("    options", key); }
        if (m.type === "float" && (m.step !== c2.step || (m.unit || null) !== (c2.unit || null))) { vizOk = false; console.log("    format", key); }
    }
    check(vizOk, "rsna: module.json's viz / extra_keys / formats match RS_DEPS, the rules and RS_META");
    check(cp.latch.viz && ["engine", "hm_tuning_mode"].every((x) => cp.latch.viz.extra_keys.indexOf(x) >= 0), "latch: reads engine and hm_tuning_mode");

    // the Remote UI dims with the very same rules
    const html = readFileSync(join(root, "src/ui/web_ui.html"), "utf8"), cj = readFileSync(join(root, "src/ui/canvas.js"), "utf8");
    const blk = (t) => { const a2 = t.indexOf("    /* RS_ACTS begin"), b2 = t.indexOf("    /* RS_ACTS end */"); return a2 >= 0 && b2 > a2 ? t.slice(a2, b2) : null; };
    check(blk(cj) !== null && blk(cj) === blk(html), "rsna: web_ui.html's RS_ACTS block is canvas.js's, character for character");
}


// ---- Resynth's Presets page (drawPage on the overlay: the host's preset_browser canvas page), the name wrapped
{
    const ovp = globalThis.canvas_overlay;
    check(typeof ovp.drawPage === "function", "presets page: the overlay exposes drawPage");
    // the host's frame for it: 120 x 45 (MENU_FRAME_W, rows 9 .. RULE_Y - 1); print is the 7-row font, 6 px a letter here
    const pgCtx = (Wp = 120, Hp = 45) => {
        const runs = []; let oob = 0;
        return { width: Wp, height: Hp, runs, get outOfBounds() { return oob; },
                 fillRect(x, y, w, h) { if (x < 0 || y < 0 || x + w > Wp || y + h > Hp) oob++; },
                 setPixel(x, y) { if (x < 0 || y < 0 || x >= Wp || y >= Hp) oob++; },
                 print(x, y, t) { const w = String(t).length * 6; runs.push({ x, y, t: String(t) }); if (x < 0 || x + w > Wp || y < 0 || y + 7 > Hp) oob++; },
                 textWidth(t) { return String(t).length * 6; } };
    };
    const runP = (preset, Wp, Hp, state) => { const k = pgCtx(Wp, Hp); ovp.drawPage(k, { preset, values: {}, state: state || {}, width: Wp, height: Hp, nowMs: 0 }); return k; };
    let k = runP({ name: "Tokyo Speirs - Inst (Scale) - Radio Feedback", index: 11, count: 101, entered: false });
    const nameRuns = k.runs.slice(0, -1);
    check(k.runs[k.runs.length - 1].t === "12 of 101", "presets page: the position under the name, counted from 1");
    check(nameRuns.length === 3 && nameRuns.map((r) => r.t).join(" ") === "Tokyo Speirs - Inst (Scale) - Radio Feedback",
          "presets page: a 44-letter name over three lines, every word kept (" + nameRuns.map((r) => r.t).join(" | ") + ")");
    check(k.outOfBounds === 0 && k.runs.every((r, i) => i === 0 || r.y > k.runs[i - 1].y + 7), "presets page: inside the frame, lines don't overlap");
    k = runP({ name: "Fifth Above", index: 0, count: 30 });
    check(k.runs.length === 2 && k.runs[0].t === "Fifth Above" && k.runs[0].x === Math.floor((120 - 66) / 2), "presets page: a short name is one centred line");
    // every imported name length up to 60 letters, at the frame and a narrower one: inside, and nothing lost while it fits
    let okAll = true;
    const words = ["Tuner", "-", "A", "Dream", "To", "Remember", "(Distort", "Me!)", "Bassliner", "Supercalifragilistic"];
    for (const Wp of [120, 96]) for (let n = 1; n <= 12; n++) {
        const name = Array.from({ length: n }, (_, i) => words[(i * 7) % words.length]).join(" ");
        const kk = runP({ name, index: 3, count: 9 }, Wp);
        const lines = kk.runs.slice(0, -1).map((r) => r.t);
        const fits = lines.length <= 3 && lines.every((l) => l.length * 6 <= Wp - 4);
        const whole = lines.join(" ").replace(/\s+/g, "") === name.replace(/\s+/g, "");
        const cut = lines.length === 3 && lines[2].endsWith("..");
        if (kk.outOfBounds || !fits || !(whole || cut)) { okAll = false; console.log("    wrap", Wp, JSON.stringify(name), JSON.stringify(lines)); }
    }
    check(okAll, "presets page: any length wraps inside the frame, whole or ending in '..'");
    const longWord = runP({ name: "Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", index: 0, count: 1 });
    check(longWord.outOfBounds === 0 && longWord.runs.length >= 3, "presets page: a word wider than the line is split, not clipped");
    check(runP({ name: "(loading)" }).runs.length === 1 && runP({ name: "(loading)" }).runs[0].t === "(loading)", "presets page: before the bank is in, the name alone");
    const stp = {};
    runP({ name: "Opera Singer", index: 1, count: 5 }, 120, 45, stp);
    check(stp.rsWrapKey && stp.rsWrapLines && stp.rsWrapLines[0] === "Opera Singer", "presets page: the wrap is cached on the page's state");
    check(runP({ name: "Robot Choir", index: 1, count: 5 }, 120, 45, stp).runs[0].t === "Robot Choir", "presets page: a new name replaces the cached wrap");
    let threwP = false;
    try {
        ovp.drawPage(pgCtx(), undefined); ovp.drawPage(pgCtx(), {}); runP(null); runP({ name: 42, index: "x", count: NaN });
        runP({ name: "", index: -3, count: 2 }); runP({ name: "A B C", index: 0, count: 1 }, 0, 0); runP({ name: "A B C", index: 0, count: 1 }, 30, 10);
        ovp.drawPage({ width: 120, height: 45, print() {}, fillRect() {} }, { preset: { name: "No textWidth here at all", index: 0, count: 1 } });
    } catch (e) { threwP = true; console.log("  threw:", e); }
    check(!threwP, "presets page: junk input never throws");
    check(runP({ name: "A B C", index: 0, count: 1 }, 30, 10).outOfBounds === 0, "presets page: a tiny frame stays inside");
    // module.json: one canvas page, the resynth level's preset browser, listed on that level
    const mjp = JSON.parse(readFileSync(join(root, "module.json"), "utf8"));
    const pgs = mjp.capabilities.chain_params.filter((p) => p.type === "canvas");
    check(pgs.length === 1 && pgs[0].key === "rs_presets_page" && pgs[0].as_page === true && pgs[0].preset_browser === true &&
          pgs[0].canvas_script === "canvas.js" && !pgs[0].enterable &&
          mjp.capabilities.ui_hierarchy.levels.resynth.params.includes("rs_presets_page") && mjp.capabilities.ui_hierarchy.levels.resynth.list_param === "rs_preset",
          "presets page: module.json declares it as the resynth level's preset browser");
}
console.log(`canvas.js: ${checks} checks, ${failures} failed`);
process.exit(failures ? 1 : 0);
