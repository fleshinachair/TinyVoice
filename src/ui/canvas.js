/*
 * TinyVoice's OLED widget for the Schwung param pages
 * (shared/param_pages/widget_registry.mjs).
 *
 * custom:tinyvoice_scope spans PITCH knobs 5-7 (the read-only Pitch, Target and
 * Cents readouts), drawn above those knobs' labels. It shows:
 *   pitch modes (Snap, Chords, Prism, Tape, Scream, HardTune, Morph, Double)
 *       the target note name, and a needle for how far the sung pitch is from
 *       it (+-50 cents across the bar); "--" when nothing is pitched (Dalek,
 *       Harmony and Walkie run no tracker, so they read "--")
 *   Robot, Nightcall   16 bars: the vocoder's analysis bands
 *   Freeze  a playhead line along the bottom while a take loops; while
 *           recording, a bar that fills as the take does, blinking head; and a
 *           tag top right: "ARM" (blinking) while waiting for the downbeat,
 *           "REC", "LOOP" (it takes the Auto readout's place)
 *   Auto    "+NN" when the input AGC is lifting a soft voice by NN dB
 *   pads    the target note drawn inverted while a pad sets it, and a dot
 *           that flashes on every note-on (shows pads are reaching the FX)
 *
 * custom:tinyvoice_freeze spans MORE knobs 3-4 (Freeze, Freeze Len). Off: the
 * two values, centred in their cells like the stock knobs. Active: the box is
 * cleared and taken over - "[ *** ARMED *** ]" flashing inverted with the beats
 * left to bar 1 as squares, an inverted "[ REC ]" with the take filling,
 * "[ LOOP 2 BARS ]" with a playhead.
 *
 * Draw hooks get cached values only: the host reads the `scope` key on its own
 * rotation (~5 Hz), so this eases the bars and needle between reads and runs
 * the loop playhead (and the recording bar) on by itself. Feed format
 * (src/dsp/dsp.c format_scope); <freeze> is 0 live, 1 looping, 2 recording,
 * 3 armed (waiting for the downbeat), and while recording <loop phase>/<loop s>
 * are how far through the take and the seconds so far:
 *   "S,<mode>,<freeze>,<loop phase>,<loop s>,<f0>,<target>,<cents>,<b0>..<b15>,
 *    <auto gain dB>,<held pad note or -1>,<note-on count>,<transport beat or -1>"
 *   (the last four optional)
 */
/* The host's own knob pictures (dial, option square, big number), so a gated knob that acts looks exactly like the stock
 * cell it replaces: a custom widget owns its cell and cannot hand it back. The same import other modules' ui.js make;
 * a missing export falls back to the value as text. tests/canvas_test.mjs swaps this line for a stub. */
import * as HOST_RENDER from "/data/UserData/schwung/shared/param_pages/render_page_movy.mjs";
(function () {
    /* the modes that draw 16 analysis bands (the vocoders: Robot 0, Nightcall 5) */
    const BAND_MODES = [0, 5];
    const isBands = () => BAND_MODES.indexOf(st.mode) >= 0;
    const NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
    const BAR_FALL_PER_MS = 255 / 400;   /* a full bar falls in 0.4 s */
    const NEEDLE_EASE_MS = 120;
    const STALE_MS = 1500;               /* no fresh read for this long: stop extrapolating */
    const FLASH_MS = 300;                /* note-on activity dot */

    const st = {
        raw: null, seenAt: 0, lastDraw: 0,
        mode: 0, frozen: false, rec: false, armed: false, phase: 0, loopS: 0,
        f0: 0, target: 0, cents: 0, shownCents: 0,
        bars: new Array(16).fill(0), shown: new Array(16).fill(0),
        agcDb: 0, pad: -1, noteOns: -1, flashAt: -1e9, beat: -1,
    };

    function noteName(hz) {
        if (!(hz > 0)) return "--";
        const m = Math.round(69 + 12 * Math.log2(hz / 440));
        return NAMES[((m % 12) + 12) % 12] + (Math.floor(m / 12) - 1);
    }

    function parse(raw, now) {
        if (typeof raw !== "string" || raw === st.raw) return;
        const f = raw.split(",");
        if (f[0] !== "S" || f.length < 24) return;
        const n = f.slice(1).map(Number);
        if (!n.every(Number.isFinite)) return;
        st.raw = raw;
        st.seenAt = now;
        st.mode = n[0] | 0;
        st.frozen = n[1] === 1;
        st.rec = n[1] === 2;
        st.armed = n[1] === 3;
        st.phase = Math.min(1, Math.max(0, n[2]));
        st.loopS = Math.max(0, n[3]);
        st.f0 = n[4];
        st.target = n[5];
        st.cents = Math.max(-50, Math.min(50, n[6]));
        for (let i = 0; i < 16; i++) st.bars[i] = Math.max(0, Math.min(255, n[7 + i]));
        if (n.length >= 26) {
            st.agcDb = n[23];
            st.pad = n[24] | 0;
            const ons = n[25] | 0;
            if (st.noteOns >= 0 && ons !== st.noteOns) st.flashAt = now;   /* a note arrived */
            st.noteOns = ons;
        }
        st.beat = n.length >= 27 ? n[26] : -1;
    }

    function midiName(note) {
        return NAMES[((note % 12) + 12) % 12] + (Math.floor(note / 12) - 1);
    }

    function ease(now) {
        const dt = st.lastDraw ? Math.min(100, Math.max(0, now - st.lastDraw)) : 0;
        st.lastDraw = now;
        for (let i = 0; i < 16; i++) {           /* rise at once, fall smoothly */
            const target = st.bars[i];
            st.shown[i] = target >= st.shown[i] ? target : Math.max(target, st.shown[i] - BAR_FALL_PER_MS * dt);
        }
        const k = dt > 0 ? 1 - Math.exp(-dt / NEEDLE_EASE_MS) : 1;
        st.shownCents += (st.cents - st.shownCents) * k;
        return dt;
    }

    /* Loop playhead, run on locally between reads like a clock. */
    function loopPhase(now) {
        if (!st.frozen || !(st.loopS > 0)) return -1;
        if (now - st.seenAt > STALE_MS) return st.phase;
        const p = st.phase + (now - st.seenAt) / (st.loopS * 1000);
        return p - Math.floor(p);
    }

    /* Recording: buffer fill 0..1, run on locally between reads. */
    function recFill(now) {
        if (!st.rec) return -1;
        if (now - st.seenAt > STALE_MS || !(st.loopS > 0) || !(st.phase > 0)) return st.phase;
        const perMs = st.phase / (st.loopS * 1000);               /* fill per ms, from fill / seconds */
        return Math.min(1, st.phase + (now - st.seenAt) * perMs);
    }

    function drawBars(ctx, top, h) {
        const W = ctx.width;
        const bw = Math.max(1, Math.floor(W / 16) - 1);
        const step = Math.floor(W / 16);
        const x0 = Math.floor((W - step * 16 + 1) / 2);
        for (let i = 0; i < 16; i++) {
            const bh = Math.round((st.shown[i] / 255) * h);
            if (bh > 0) ctx.fillRect(x0 + i * step, top + h - bh, bw, bh, 1);
            else ctx.fillRect(x0 + i * step, top + h - 1, bw, 1, 1);   /* floor tick: the meter is alive */
        }
    }

    function drawPitch(ctx, top, h, right) {
        const W = ctx.width;
        const lw = typeof ctx.textWidth === "function" ? ctx.textWidth("C#4") : 18;
        const ty = top + Math.max(0, Math.floor((h - 6) / 2));
        if (st.pad >= 0) {                                        /* a pad sets the target: inverted label */
            ctx.fillRect(0, top, lw + 2, h, 1);
            ctx.print(1, ty, midiName(st.pad), 0);
        } else {
            ctx.print(0, ty, st.f0 > 0 ? noteName(st.target) : "--", 1);
        }
        const x0 = lw + 4, x1 = W - 1 - right;
        const mid = Math.round((x0 + x1) / 2), half = Math.floor((x1 - x0) / 2);
        const cy = top + Math.floor(h / 2);
        ctx.fillRect(x0, cy, x1 - x0 + 1, 1, 1);                      /* scale line */
        ctx.fillRect(mid, top, 1, h, 1);                              /* in tune */
        for (const c of [-25, 25]) ctx.fillRect(mid + Math.round((c / 50) * half), cy - 1, 1, 3, 1);
        if (st.f0 > 0) {
            const nx = mid + Math.round((st.shownCents / 50) * half);
            ctx.fillRect(Math.min(mid, nx), cy - 1, Math.abs(nx - mid) + 1, 3, 1);   /* the error */
            ctx.fillRect(nx - 1, top, 3, h, 1);                                      /* the needle */
        }
    }

    function draw(ctx, payload) {
        const p = payload || {};
        const values = p.values || {};
        const now = typeof p.nowMs === "number" ? p.nowMs : Date.now();
        parse(values.scope, now);
        ease(now);
        if (!st.raw) return;

        const H = ctx.height, W = ctx.width;
        const lp = loopPhase(now), rf = recFill(now);
        const body = lp >= 0 || rf >= 0 ? H - 3 : H;                  /* room for the loop line */
        const tw = (s) => (typeof ctx.textWidth === "function" ? ctx.textWidth(s) : s.length * 6);
        /* Freeze's state tag, or (otherwise) Auto's boost, top right */
        const freezeOn = st.armed || st.rec || st.frozen;
        const tag = st.armed ? (Math.floor(now / 250) % 2 === 0 ? "ARM" : "") : st.rec ? "REC" : st.frozen ? "LOOP" : "";
        const agcTxt = !freezeOn && st.agcDb >= 1 ? "+" + Math.round(st.agcDb) : "";
        const right = freezeOn ? tw("LOOP") + 2 : agcTxt ? tw("+24") + 2 : 0;
        if (isBands()) drawBars(ctx, 0, body);
        else drawPitch(ctx, 0, body, right);
        const topRight = tag || agcTxt;                               /* Freeze's state, or Auto's lift */
        if (topRight) {
            const x = W - tw(topRight);
            if (isBands()) ctx.fillRect(x - 1, 0, W - x + 1, 7, 0);
            ctx.print(x, 0, topRight, 1);
        }
        if (now - st.flashAt < FLASH_MS) ctx.fillRect(W - 2, H - (body < H ? 5 : 2), 2, 2, 1);  /* note-on */

        if (lp >= 0) {
            for (let x = 0; x < W; x += 2) ctx.fillRect(x, H - 1, 1, 1, 1);   /* the loop, dotted */
            const px = Math.round(lp * (W - 1));
            ctx.fillRect(0, H - 1, px + 1, 1, 1);                              /* played so far */
            ctx.fillRect(Math.max(0, px - 1), H - 3, 3, 3, 1);                 /* playhead */
        }
        if (rf >= 0) {
            const px = Math.round(rf * (W - 1));
            ctx.fillRect(0, H - 2, px + 1, 2, 1);                              /* recorded so far */
            if (Math.floor(now / 250) % 2 === 0)
                ctx.fillRect(Math.min(W - 3, px), H - 3, 3, 3, 1);             /* blinking head */
        }
    }

    /* ---- Freeze banner (MORE knobs 3-4) ----
     *
     * The frame is the two cells' graphic area, above the host's label band
     * ("FREEZE", "FRELEN"). The host clips text at the frame's right edge but
     * not its bottom, so text sits on ONE line, vertically centred, and the
     * rest (countdown, take, playhead) is drawn as graphics on the bottom rows:
     * a second text line used to run into the label band, and two strings on
     * one line ran into each other ("FREE2E"). */
    const LEN_OPTS = ["Free", "1 Bar", "2 Bars", "4 Bars"];
    const FREEZE_OPTS = ["Off", "Rec", "Loop"];
    function optName(v, opts, dflt) {
        if (typeof v === "string") {
            const t = v.trim();
            if (opts.indexOf(t) >= 0) return t;
            const n = Number(t);
            if (t !== "" && Number.isInteger(n) && n >= 0 && n < opts.length) return opts[n];
            return dflt;
        }
        return Number.isInteger(v) && v >= 0 && v < opts.length ? opts[v] : dflt;
    }
    const TEXT_H = 8;                                                 /* the display font, with room */

    function drawFreeze(ctx, payload) {
        const p = payload || {};
        const values = p.values || {};
        const now = typeof p.nowMs === "number" ? p.nowMs : Date.now();
        parse(values.scope, now);
        const W = ctx.width, H = ctx.height;
        const tw = (s) => (typeof ctx.textWidth === "function" ? ctx.textWidth(s) : s.length * 6);
        const ty = Math.max(0, Math.min(H - TEXT_H, Math.floor((H - TEXT_H - 3) / 2)));
        const fit = (cands) => { for (const c of cands) if (tw(c) <= W - 2) return c; return ""; };
        const centre = (txt, color) => { if (txt) ctx.print(Math.max(0, Math.floor((W - tw(txt)) / 2)), ty, txt, color); };
        const len = optName(values.freeze_len, LEN_OPTS, "2 Bars");
        const clock = st.raw && st.beat >= 0 && now - st.seenAt < STALE_MS;

        if (!(st.armed || st.rec || st.frozen)) {
            /* OFF: what the stock knobs would show - each value, centred in its
             * own cell. No banner, no extra strings. */
            /* Each centred in its cell; a value wider than its cell borrows
             * room from beside the other (3 px apart, never over it), and only
             * if the two can't both fit is the length shortened. */
            const half = Math.floor(W / 2), GAP = 3;
            const left = optName(values.freeze, FREEZE_OPTS, "Off");
            let right = len;
            while (right.length > 1 && tw(left) + GAP + tw(right) > W) right = right.slice(0, -1);
            let lx = Math.max(0, Math.floor((half - tw(left)) / 2));
            let rx = half + Math.floor((W - half - tw(right)) / 2);
            if (rx + tw(right) > W) rx = W - tw(right);
            if (rx < lx + tw(left) + GAP) {
                rx = Math.max(rx, Math.min(W - tw(right), lx + tw(left) + GAP));
                lx = Math.max(0, Math.min(lx, rx - GAP - tw(left)));
            }
            ctx.print(lx, ty, left, 1);
            ctx.print(rx, ty, right, 1);
            return;
        }

        ctx.fillRect(0, 0, W, H, 0);                                  /* clear the box first */
        if (st.armed) {
            /* ARMED: flashes inverted <-> outlined at 4 Hz; the beats left to
             * bar 1 as filled squares along the bottom */
            const on = Math.floor(now / 125) % 2 === 0;
            const fg = on ? 0 : 1;
            if (on) ctx.fillRect(0, 0, W, H, 1);
            else { ctx.fillRect(0, 0, W, 1, 1); ctx.fillRect(0, H - 1, W, 1, 1); ctx.fillRect(0, 0, 1, H, 1); ctx.fillRect(W - 1, 0, 1, H, 1); }
            centre(fit(["[ *** ARMED *** ]", "*** ARMED ***", "* ARMED *", "ARMED"]), fg);
            if (clock) {
                const left = Math.max(1, Math.min(4, Math.ceil(4 - (((st.beat % 4) + 4) % 4) - 1e-6)));
                const sq = 4, gap = 3, total = 4 * sq + 3 * gap, x0 = Math.floor((W - total) / 2);
                for (let k = 0; k < 4; k++) {
                    const x = x0 + k * (sq + gap);
                    if (k < left) ctx.fillRect(x, H - 3, sq, 2, fg);
                    else ctx.fillRect(x, H - 2, sq, 1, fg);
                }
            }
            return;
        }
        if (st.rec) {
            /* REC: solid inverted, a dot, the take filling along the bottom */
            ctx.fillRect(0, 0, W, H, 1);
            const txt = fit(["[ REC ]", "REC"]);
            const tx = Math.max(6, Math.floor((W - tw(txt) + 6) / 2));
            ctx.fillRect(tx - 6, ty + 2, 4, 4, 0);                    /* the record dot */
            ctx.print(tx, ty, txt, 0);
            const f = recFill(now);
            if (f > 0) ctx.fillRect(1, H - 2, Math.max(1, Math.round(f * (W - 2))), 1, 0);
            return;
        }
        /* LOOP: the length, and the playhead along the bottom */
        centre(fit(["[ LOOP " + len.toUpperCase() + " ]", "[ LOOP ]", "LOOP"]), 1);
        ctx.fillRect(0, H - 2, W, 1, 1);
        const lp = loopPhase(now);
        if (lp >= 0) ctx.fillRect(Math.max(0, Math.round(lp * (W - 3))), H - 3, 3, 3, 1);
    }

    /* custom:tinyvoice_latch, MORE knob 6. In the modes Latch acts in (Robot, Nightcall, Harmony,
     * Chords, Prism) the value as the stock enum knob would show it, On
     * inverted so a held chord is seen at a glance; in the others a dimmed
     * "---" (every other pixel cleared: the 1-bit screen's grey). The cell,
     * its name and the knob never move; the knob still turns everywhere. Until
     * the mode has been read it shows the value. */
    const MODES = ["Robot", "Snap", "Chords", "Prism", "Tape", "Nightcall", "Scream", "Dalek",
                   "HardTune", "Harmony", "Walkie", "Morph", "Double"];
    const LATCH_MODES = ["Robot", "Chords", "Prism", "Nightcall", "Harmony"];
    function drawLatch(ctx, payload) {
        const values = (payload && payload.values) || {};
        const W = ctx.width, H = ctx.height;
        const tw = (s) => (typeof ctx.textWidth === "function" ? ctx.textWidth(s) : s.length * 6);
        const ty = Math.max(0, Math.min(H - TEXT_H, Math.floor((H - TEXT_H - 3) / 2)));
        const engine = optName(values.engine, ["Vocoder", "Synth Voice", "Resynth"], null);
        const mode = optName(values.mode, MODES, null);
        const na = engine === "Resynth" ? RS_ACTS.latch((k) => values[k]) === false : !!mode && LATCH_MODES.indexOf(mode) < 0;
        if (na) {
            const txt = "---", x = Math.max(0, Math.floor((W - tw(txt)) / 2));
            ctx.print(x, ty, txt, 1);
            if (typeof ctx.setPixel === "function")
                for (let y = ty; y < Math.min(H, ty + TEXT_H); y++)
                    for (let xx = x; xx < Math.min(W, x + tw(txt)); xx++)
                        if ((xx + y) & 1) ctx.setPixel(xx, y, 0);
            return;
        }
        const on = optName(values.latch, ["Off", "On"], "Off") === "On";
        const txt = on ? "On" : "Off", x = Math.max(0, Math.floor((W - tw(txt)) / 2));
        if (on) ctx.fillRect(Math.max(0, x - 2), Math.max(0, ty - 1), Math.min(W, tw(txt) + 4), Math.min(H, TEXT_H + 1), 1);
        ctx.print(x, ty, txt, on ? 0 : 1);
    }

    /* The stock picture for a knob that acts, from the host's renderer and its metadata for the key (payload.metaIndex):
     * widgetKindFor picks a dial (a range), an option square (an enum) or a big number (a small whole-number range),
     * placed as drawKnobWidget places it in the cell. False when the host gave no metadata or the renderer lacks a
     * function (the caller then prints the value). */
    function drawStock(ctx, payload, key, raw) {
        const R = typeof HOST_RENDER === "object" && HOST_RENDER ? HOST_RENDER : null;
        if (!R || typeof R.widgetKindFor !== "function") return false;
        try {
            const mi = payload && payload.metaIndex;
            const meta = mi && typeof mi.getOrGuess === "function" ? mi.getOrGuess(key) : null;
            if (!meta || typeof meta !== "object") return false;
            const W = ctx.width | 0, kind = R.widgetKindFor(meta);
            if (kind === "enum" && typeof R.drawEnumSquare === "function") {
                const opts = Array.isArray(meta.options) ? meta.options : [];
                let idx = opts.indexOf(raw);
                if (idx < 0 && typeof raw === "string") idx = opts.findIndex((o) => String(o).toLowerCase() === raw.toLowerCase());
                if (idx < 0) { const n = Number(raw); if (raw !== "" && raw !== null && Number.isInteger(n) && n >= 0 && n < opts.length) idx = n; }
                const so = Array.isArray(meta.short_options) ? meta.short_options : null;
                const text = so && so[idx] !== undefined ? String(so[idx]) : (opts[idx] !== undefined ? String(opts[idx]) : (raw === undefined || raw === null ? "--" : String(raw)));
                R.drawEnumSquare(ctx, Math.floor((W - (Number(R.ENUM_W) || 28)) / 2), 0, text, null, undefined, null, raw, false);
                return true;
            }
            if (kind === "bignum" && typeof R.drawBigNumber === "function" && typeof R.bigNumberText === "function") {
                R.drawBigNumber(ctx, Math.floor(W / 2), 0, R.bigNumberText(meta, raw));
                return true;
            }
            if (kind === "knob" && typeof R.drawArcKnob === "function") {
                const nv = typeof R.normalizedOf === "function" ? R.normalizedOf(meta, raw) : null;
                R.drawArcKnob(ctx, Math.floor((W - (Number(R.KW) || 17)) / 2), 0, typeof nv === "number" && isFinite(nv) ? nv : 0);
                return true;
            }
        } catch (e) { /* the text below instead */ }
        return false;
    }

    /* The knobs that do nothing in some modes: Key, Scale, Retune,
     * Glide, Beauty (all on PITCH). Where a knob acts the widget shows its value as
     * the stock knob would; where it doesn't, a dimmed "N/A" (every other
     * pixel cleared: the 1-bit screen's grey). Same cell, name and knob in
     * every mode, and the DSP ignores turns where the knob does nothing. The
     * "acts" lists are the same table as vt_param_acts in dsp.h (both test
     * suites check the same expected table). Until the mode has been read it shows the value.
     * Candidates run longest to shortest; the first that fits the cell wins. */
    const ROOTS = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
    const SCALES = ["Chromatic", "Major", "Minor", "Pentatonic", "Dorian"];
    const SCALE_SHORT = { Chromatic: ["Chromatic", "Chrom", "Chr"], Pentatonic: ["Pentatonic", "Penta", "Pen"], Dorian: ["Dorian", "Dor"] };
    const ms = (v) => { const n = Number(v), r = Number.isFinite(n) ? Math.round(Math.max(0, Math.min(500, n))) : 0; return [r + "ms", String(r)]; };
    const GATED = {
        key:    { acts: ["Robot", "Snap", "Chords", "Prism", "Nightcall", "HardTune", "Morph", "Double"],
                  text: (v) => [optName(v, ROOTS, "C")] },
        scale:  { acts: ["Snap", "Chords", "Prism", "HardTune", "Morph", "Double"],
                  text: (v) => { const n = optName(v, SCALES, "Chromatic"); return SCALE_SHORT[n] || [n]; } },
        retune: { acts: ["Snap", "Chords", "Morph", "Double"], text: ms },
        glide:  { acts: ["Robot", "Snap", "Chords", "Prism", "Nightcall", "HardTune", "Morph", "Double"], text: ms },
        beauty: { acts: ["Robot", "Snap", "Chords", "Prism"],
                  text: (v) => { const n = Number(v); return [(Number.isFinite(n) ? Math.round(Math.max(0, Math.min(1, n)) * 100) : 0) + "%"]; } },
    };
    function drawGated(ctx, payload, key) {
        const g = GATED[key];
        const values = (payload && payload.values) || {};
        const W = ctx.width, H = ctx.height;
        const tw = (s) => (typeof ctx.textWidth === "function" ? ctx.textWidth(s) : s.length * 6);
        const ty = Math.max(0, Math.min(H - TEXT_H, Math.floor((H - TEXT_H - 3) / 2)));
        const mode = optName(values.mode, MODES, null);
        const dim = !!mode && g.acts.indexOf(mode) < 0;
        if (!dim && drawStock(ctx, payload, key, values[key])) return;
        const cands = dim ? ["N/A"] : g.text(values[key]);
        let txt = cands[cands.length - 1];
        for (const c of cands) if (tw(c) <= W - 2) { txt = c; break; }
        while (tw(txt) > W - 2 && txt.length > 1) txt = txt.slice(0, -1);   /* a narrow cell: clip the shortest form */
        const x = Math.max(0, Math.floor((W - tw(txt)) / 2));
        ctx.print(x, ty, txt, 1);
        if (dim && typeof ctx.setPixel === "function")
            for (let y = ty; y < Math.min(H, ty + TEXT_H); y++)
                for (let xx = x; xx < Math.min(W, x + tw(txt)); xx++)
                    if ((xx + y) & 1) ctx.setPixel(xx, y, 0);
    }
    const gatedWidget = (key) => ({ draw: (ctx, payload) => drawGated(ctx, payload, key), nominal: { w: 32, h: 15 } });

    /* Resynth's knobs that do nothing in the current settings (custom:tinyvoice_rsna; one group per knob, the
     * cell's key in payload.group.keys). Where a knob acts it shows its value exactly as the stock knob would (fmtValue is the host's
     * shared/param_format.mjs, for the units used here); where it doesn't, a dimmed "N/A". Unlike the vocoder's,
     * the DSP still takes turns of an N/A knob (the value shows in the header while turned): imports and
     * restores write keys one by one in any order. RS_ACTS is the one table of rules; web_ui.html dims its
     * controls with the same rules (rsActs there; tests/canvas_test.mjs checks both). Until the keys a rule reads
     * have been read, the value shows. */
    const RS_META = {
        hm_transform:         { type: "float", step: 0.01 },
        hm_pitch_shift:       { type: "float", step: 1, unit: "st" },
        hm_octaving:          { type: "float", step: 0.01 },
        hm_wave:              { type: "enum", options: ["Saw", "Square", "Triangle", "Sine", "Preset"] },
        hm_wavetable_shape:   { type: "float", step: 0.01 },
        hm_wavetable_stretch: { type: "float", step: 0.01 },
        hm_key:               { type: "enum", options: ROOTS },
        hm_scale:             { type: "enum", options: ["Major", "Minor", "Chromatic", "Custom"] },
        hm_tuning_duophonic:  { type: "enum", options: ["Off", "On"] },
        hm_pitch_mono:        { type: "note" },
        hm_pitch_duo:         { type: "note" },
        hm_midi_idle:         { type: "enum", options: ["Tune", "Silent"] },
        latch:                { type: "enum", options: ["Off", "On"] },
        hm_warble_rate:       { type: "float", step: 0.01 },
        hm_warble_mode:       { type: "enum", options: ["FM", "LFO"] },
        hm_filter_freq:       { type: "float", step: 0.5 },
        hm_output_gain:       { type: "float", step: 0.1, unit: "dB" },
        hm_freeze_synclength: { type: "enum", options: ["1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1/1"] },
    };
    function precisionForStep(step) {
        const s = Math.abs(Number(step));
        return !isFinite(s) || s <= 0 ? 2 : s >= 1 ? 0 : s >= 0.1 ? 1 : s >= 0.01 ? 2 : s >= 0.001 ? 3 : 4;
    }
    function fmtValue(raw, meta) {
        if (meta.type === "enum") return optName(raw, meta.options, String(raw));
        const n = Number(raw);
        if (!isFinite(n)) return String(raw);
        if (meta.type === "note") { const m = Math.round(n); return NOTE_NAMES[((m % 12) + 12) % 12] + (Math.floor(m / 12) - 1); }
        if (meta.type === "int" && !meta.unit) return String(Math.round(n));
        if (meta.unit === "dB") return n.toFixed(precisionForStep(meta.step)) + " dB";
        if (meta.unit === "Hz") return Math.abs(n) >= 1000 ? (n / 1000).toFixed(2) + " kHz" : n.toFixed(precisionForStep(meta.step)) + " Hz";
        if (meta.unit === "ms") return n.toFixed(precisionForStep(meta.step)) + " ms";
        if (meta.unit === "st") { const r = Math.round(n); return (r > 0 ? "+" : "") + r + " st"; }
        return n.toFixed(precisionForStep(meta.step)) + (meta.unit ? " " + meta.unit : "");
    }
    const NOTE_NAMES = ROOTS;
    /* RS_ACTS begin: the same text in src/ui/canvas.js and src/ui/web_ui.html (tests/canvas_test.mjs checks it).
     * Which Resynth knobs act in the current settings. get(key) is the raw value as read (an option name, an
     * index or a number; undefined until read); a rule answers false (N/A), true (acts) or null (not known yet). */
    const RS_ACTS = (function () {
        const MODE = ["Scale", "Note", "MIDI"], OFFON = ["Off", "On"], IDLE = ["Tune", "Silent"];
        const read = (v) => v === undefined || v === null || v === "";
        const num = (g, k) => { const v = g(k), n = Number(v); return read(v) || !isFinite(n) ? null : n; };
        const opt = (g, k, opts) => {
            const v = g(k);
            if (read(v)) return null;
            const s = String(v);
            for (const o of opts) if (o.toLowerCase() === s.toLowerCase()) return o;
            const n = Number(s);
            return isFinite(n) && n >= 0 && n < opts.length ? opts[Math.round(n)] : null;
        };
        const and = (...xs) => xs.some((x) => x === false) ? false : xs.some((x) => x === null) ? null : true;
        const gt = (n, t) => n === null ? null : n > t;
        const is = (x, v) => x === null ? null : x === v;
        const mode = (g) => opt(g, "hm_tuning_mode", MODE);
        const pathB = (g) => gt(num(g, "hm_synth_mix"), 0.005);                      /* Shift, Octave, Transform: path B */
        const transform = (g) => and(pathB(g), gt(num(g, "hm_transform"), 0.005));   /* the wave acts only through it */
        const scale = (g) => {      /* Scale mode; MIDI mode with no pad held when No Pad is Tune; Bypass: only a shifted line snaps */
            const m = mode(g);
            if (m === null) return null;
            if (m === "Note") return false;
            if (m === "MIDI") return is(opt(g, "hm_midi_idle", IDLE), "Tune");
            const byp = opt(g, "hm_quantize_enabled", OFFON);
            if (byp === null) return null;
            if (byp === "Off") return true;
            const sh = num(g, "hm_pitch_shift");
            return and(sh === null ? null : Math.abs(sh) > 0.01, pathB(g));
        };
        const note = (g) => is(mode(g), "Note"), midi = (g) => is(mode(g), "MIDI");
        const warble = (g) => gt(num(g, "hm_warble_depth"), 0.0);
        return {
            hm_transform: pathB, hm_pitch_shift: pathB, hm_octaving: pathB,
            hm_wave: transform, hm_wavetable_shape: transform, hm_wavetable_stretch: transform,
            hm_chebyshev: (g) => and(transform(g), gt(num(g, "hm_wavetable_shape"), 0.0)),   /* Odd / Even: through Shape */
            hm_key: scale, hm_scale: scale, hm_mask: scale,
            hm_quantize_enabled: (g) => { const m = mode(g); return m === null ? null : m === "Scale" || (m === "MIDI" ? is(opt(g, "hm_midi_idle", IDLE), "Tune") : false); },
            hm_tuning_duophonic: note, hm_pitch_mono: note,
            hm_pitch_duo: (g) => and(note(g), is(opt(g, "hm_tuning_duophonic", OFFON), "On")),
            hm_midi_idle: midi, latch: midi,
            hm_warble_rate: warble,
            hm_warble_mode: (g) => { const r = num(g, "hm_warble_rate"); return and(warble(g), r === null ? null : r >= 0); },
            hm_filter_freq: (g) => { const n = num(g, "hm_filter_gain"); return n === null ? null : Math.abs(n) > 0.01; },
            hm_filter_width: (g) => { const n = num(g, "hm_filter_gain"); return n === null ? null : Math.abs(n) > 0.01; },
            hm_output_gain: (g) => is(opt(g, "hm_autolevel", OFFON), "Off"),
            hm_freeze_synclength: (g) => is(opt(g, "hm_freeze_sync", OFFON), "On"),
            hm_freeze_length: (g) => is(opt(g, "hm_freeze_sync", OFFON), "Off"),
            hm_noisegate_attack: (g) => gt(num(g, "hm_noisegate_threshold"), -79.5),
            hm_noisegate_hold: (g) => gt(num(g, "hm_noisegate_threshold"), -79.5),
            hm_noisegate_release: (g) => gt(num(g, "hm_noisegate_threshold"), -79.5),
            hm_deess_sensitivity: (g) => gt(num(g, "hm_deess"), 0.05),
            hm_deess_attack: (g) => gt(num(g, "hm_deess"), 0.05),
            hm_widen_cutoff: (g) => gt(num(g, "hm_widen"), 0.0),
            hm_widen_rate: (g) => gt(num(g, "hm_widen"), 0.0),
            hm_sharpen_bias: (g) => gt(num(g, "hm_sharpen"), 0.0),
        };
    })();
    /* RS_ACTS end */
    /* the keys each rule the Move draws reads (the widgets' extra_keys: scripts/gen_module_json.py's RS_DEPS) */
    const RS_DEPS = {
        hm_transform: ["hm_synth_mix"], hm_pitch_shift: ["hm_synth_mix"], hm_octaving: ["hm_synth_mix"],
        hm_wave: ["hm_synth_mix", "hm_transform"], hm_wavetable_shape: ["hm_synth_mix", "hm_transform"], hm_wavetable_stretch: ["hm_synth_mix", "hm_transform"],
        hm_key: ["hm_tuning_mode", "hm_midi_idle", "hm_quantize_enabled", "hm_pitch_shift", "hm_synth_mix"],
        hm_scale: ["hm_tuning_mode", "hm_midi_idle", "hm_quantize_enabled", "hm_pitch_shift", "hm_synth_mix"],
        hm_tuning_duophonic: ["hm_tuning_mode"], hm_pitch_mono: ["hm_tuning_mode"], hm_pitch_duo: ["hm_tuning_mode", "hm_tuning_duophonic"],
        hm_midi_idle: ["hm_tuning_mode"],
        hm_warble_rate: ["hm_warble_depth"], hm_warble_mode: ["hm_warble_depth", "hm_warble_rate"],
        hm_filter_freq: ["hm_filter_gain"], hm_output_gain: ["hm_autolevel"], hm_freeze_synclength: ["hm_freeze_sync"],
    };
    function drawText(ctx, cands, dim) {
        const W = ctx.width, H = ctx.height;
        const tw = (s) => (typeof ctx.textWidth === "function" ? ctx.textWidth(s) : s.length * 6);
        const ty = Math.max(0, Math.min(H - TEXT_H, Math.floor((H - TEXT_H - 3) / 2)));
        let txt = cands[cands.length - 1];
        for (const c of cands) if (tw(c) <= W - 2) { txt = c; break; }
        while (tw(txt) > W - 2 && txt.length > 1) txt = txt.slice(0, -1);
        const x = Math.max(0, Math.floor((W - tw(txt)) / 2));
        ctx.print(x, ty, txt, 1);
        if (dim && typeof ctx.setPixel === "function")
            for (let y = ty; y < Math.min(H, ty + TEXT_H); y++)
                for (let xx = x; xx < Math.min(W, x + tw(txt)); xx++)
                    if ((xx + y) & 1) ctx.setPixel(xx, y, 0);
    }
    function drawRs(ctx, payload) {
        const values = (payload && payload.values) || {};
        const g = payload && payload.group;                       /* the host passes { keys: [the cell's key] } */
        const key = typeof g === "string" ? g : (g && Array.isArray(g.keys) ? g.keys[0] : undefined);
        const own = RS_META[key], rule = RS_ACTS[key];
        if (!own) return;
        /* the host's own metadata when it hands it over (metaIndex.getOrGuess), so the text is the stock knob's */
        let meta = own;
        try {
            const mi = payload.metaIndex, hm = mi && typeof mi.getOrGuess === "function" ? mi.getOrGuess(key) : null;
            if (hm && own.type !== "note" && (hm.type === "enum" ? Array.isArray(hm.options) : true)) meta = hm;
        } catch (e) { meta = own; }
        const acts = rule ? rule((k) => values[k]) : true;
        if (acts === false) { drawText(ctx, ["N/A"], true); return; }
        if (drawStock(ctx, payload, key, values[key])) return;
        const full = values[key] === undefined ? "" : fmtValue(values[key], meta);
        const sp = full.lastIndexOf(" ");                              /* "-6.0 dB" -> "-6.0dB" -> "-6.0" where the cell is narrow */
        drawText(ctx, sp > 0 ? [full, full.slice(0, sp) + full.slice(sp + 1), full.slice(0, sp)] : [full], false);
    }
    const drawBeauty = (ctx, payload) => drawGated(ctx, payload, "beauty");

    /* ---- Resynth's Presets page, drawn by us (the host's "preset_browser" canvas page) so a long name wraps.
     * The host still owns the browsing (the wheel scrolls, click enters, the knobs stay on the level) and hands
     * the state over as payload.preset = { name, index, count, entered }; its stock page shows one line in a big
     * face, which cut "Tokyo Speirs - Inst (Scale) - Radio Feedback" to its first 13 letters. Here: the name
     * word-wrapped over up to three lines, centred, "12 of 61" under it. */
    const PRESET_LINES = 3, LINE_H = 9;
    function wrapName(name, W, tw, maxLines) {
        const words = String(name).split(/\s+/).filter((w) => w.length);
        const lines = [];
        let cur = "", cut = false;
        const push = (l) => { if (lines.length < maxLines) lines.push(l); else cut = true; };
        for (let w of words) {
            while (tw(w) > W && w.length > 1) {                       /* a word wider than the line: split it */
                let n = w.length - 1;
                while (n > 1 && tw(w.slice(0, n)) > W) n--;
                if (cur) { push(cur); cur = ""; }
                push(w.slice(0, n));
                w = w.slice(n);
            }
            const t = cur ? cur + " " + w : w;
            if (tw(t) <= W) cur = t;
            else { push(cur); cur = w; }
        }
        if (cur) push(cur);
        if (cut && lines.length) {                                    /* more than fits: end the last line with ".." */
            let l = lines[lines.length - 1];
            while (l.length > 1 && tw(l + "..") > W) l = l.slice(0, -1);
            lines[lines.length - 1] = l.replace(/\s+$/, "") + "..";
        }
        return lines;
    }
    function drawPresetPage(ctx, payload) {
        const W = ctx.width | 0, H = ctx.height | 0;
        if (!(W > 8 && H > TEXT_H)) return;
        const tw = (s) => (typeof ctx.textWidth === "function" ? ctx.textWidth(s) : s.length * 6);
        const pr = (payload && payload.preset) || {};
        const name = typeof pr.name === "string" && pr.name.length ? pr.name : "--";
        const count = Number(pr.count), index = Number(pr.index);
        const pos = Number.isFinite(count) && count > 0 && Number.isFinite(index) ? `${Math.max(0, index | 0) + 1} of ${count | 0}` : "";
        const posH = pos ? LINE_H + 2 : 0;                            /* two rows of air above the position */
        const room = Math.max(1, Math.min(PRESET_LINES, Math.floor((H - posH + LINE_H - TEXT_H) / LINE_H)));
        /* the wrap is cached on the page's state (per slot) by name and width: the page redraws every tick */
        const st = payload && payload.state && typeof payload.state === "object" ? payload.state : {};
        const ck = W + "|" + room + "|" + name;
        let lines = st.rsWrapKey === ck ? st.rsWrapLines : null;
        if (!lines) { lines = wrapName(name, W - 4, tw, room); st.rsWrapKey = ck; st.rsWrapLines = lines; }
        const blockH = (lines.length - 1) * LINE_H + TEXT_H + posH;
        let y = Math.max(0, Math.floor((H - blockH) / 2));
        for (const l of lines) {
            ctx.print(Math.max(0, Math.floor((W - tw(l)) / 2)), y, l, 1);
            y += LINE_H;
        }
        y += 2;
        if (pos && y + TEXT_H <= H) ctx.print(Math.max(0, Math.floor((W - tw(pos)) / 2)), y, pos, 1);
    }

    globalThis.canvas_overlay = {
        drawPage: drawPresetPage,
        widgetKinds: {
            "custom:tinyvoice_key": gatedWidget("key"),
            "custom:tinyvoice_scale": gatedWidget("scale"),
            "custom:tinyvoice_retune": gatedWidget("retune"),
            "custom:tinyvoice_glide": gatedWidget("glide"),
            "custom:tinyvoice_beauty": gatedWidget("beauty"),
            "custom:tinyvoice_scope": { draw, nominal: { w: 96, h: 15 } },
            "custom:tinyvoice_freeze": { draw: drawFreeze, nominal: { w: 64, h: 15 } },
            "custom:tinyvoice_latch": { draw: drawLatch, nominal: { w: 32, h: 15 } },
            "custom:tinyvoice_rsna": { draw: drawRs, nominal: { w: 32, h: 15 } },
        },
    };
    /* test seam (tests/canvas_test.mjs) */
    globalThis.TINYVOICE_SCOPE_FOR_TEST = { draw, drawFreeze, drawLatch, drawBeauty, drawGated, GATED, parse, loopPhase, recFill, noteName, st, drawRs, RS_ACTS, RS_DEPS, RS_META, fmtValue, drawPresetPage, wrapName, drawStock };
})();
