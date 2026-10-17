// Builds the touch HUD's arc plate (WebTouch.cpp: the stone fan behind the thumb arc of action buttons) from the stock
// main action bar, UI-MainMenuBar-Dwarf: its plain stone and its top frame bent round the arc's pivot. The texture is
// Blizzard's, so the plate is written next to the game files, never committed:
//
//   node Tools/Bridge/make-arc-plate.mjs [--game bin/Win]
//
// Writes <game>/Interface/TouchUI/ArcPlate.tga: 512 x 512 for a 288 x 288 UI-unit square in the screen's bottom right
// corner, the pivot at pixel (370, 370), the fan from 40 to 205 units out between 85 and 220 degrees (counterclockwise
// from +x, y up), a recessed hub under the main button.

import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { decodeBlp, openGameData } from "./game-data.mjs";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const argv = process.argv.slice(2);
const gameDir = resolve(root, argv.indexOf("--game") >= 0 ? argv[argv.indexOf("--game") + 1] : "bin/Win");

const readFile = await openGameData(gameDir);
const blp = readFile("Interface\\MainMenuBar\\UI-MainMenuBar-Dwarf.blp");
if (!blp) {
    console.error("make-arc-plate: UI-MainMenuBar-Dwarf is not in the game data");
    process.exit(1);
}
const bar = decodeBlp(blp);   // 256 x 256: four 64-texel strips of the bar, 1 texel to the UI unit

// The second strip (texels 64-127) is the bar under the bag slots: the XP bar's frame (a double rail with a joint
// every 51.2 texels, the first at x 1.5) over plain stone in the shadow of the frame.
const FRAME = 72, FRAME_END = 87;   // the frame's rows, the outer edge first
const SHADOW_END = 98;              // the frame's shadow on the stone
const STONE = [52, 98, 256, 126];   // x0, y0, x1, y1 of plain stone, clear of the bag slots
const JOINT = 51.2, JOINT_X = 1.5;

function texel(x, y) {   // bilinear, x wrapping round the strip
    const x0 = Math.floor(x - 0.5), y0 = Math.floor(y - 0.5), fx = x - 0.5 - x0, fy = y - 0.5 - y0;
    const out = [0, 0, 0, 0];
    for (const [dx, dy, w] of [[0, 0, (1 - fx) * (1 - fy)], [1, 0, fx * (1 - fy)], [0, 1, (1 - fx) * fy], [1, 1, fx * fy]]) {
        const tx = (((x0 + dx) % 256) + 256) % 256, ty = Math.max(0, Math.min(255, y0 + dy)), o = (ty * 256 + tx) * 4;
        for (let i = 0; i < 4; i++) out[i] += bar.rgba[o + i] * w;
    }
    return out;
}

// The stone with the bar's lighting taken out (each row and column brought to the patch's mean), so it can be repeated
// over the whole fan without banding. Mirrored at its edges, so the repeats meet without seams.
const [sx0, sy0, sx1, sy1] = STONE, sw = sx1 - sx0, sh = sy1 - sy0;
const stone = new Float32Array(sw * sh * 3);
{
    const lum = (x, y) => { const o = ((sy0 + y) * 256 + sx0 + x) * 4; return (bar.rgba[o] + bar.rgba[o + 1] + bar.rgba[o + 2]) / 3; };
    const rows = [...Array(sh)].map((_, y) => [...Array(sw)].reduce((a, _, x) => a + lum(x, y), 0) / sw);
    const cols = [...Array(sw)].map((_, x) => [...Array(sh)].reduce((a, _, y) => a + lum(x, y), 0) / sh);
    const smooth = (v, k) => v.map((_, i) => { let a = 0, n = 0; for (let j = i - k; j <= i + k; j++) if (v[j] !== undefined) a += v[j], n++; return a / n; });
    const colsSmooth = smooth(cols, 12), mean = rows.reduce((a, b) => a + b) / sh;
    for (let y = 0; y < sh; y++) {
        for (let x = 0; x < sw; x++) {
            const k = mean * mean / (rows[y] * colsSmooth[x]), o = ((sy0 + y) * 256 + sx0 + x) * 4;
            for (let i = 0; i < 3; i++) stone[(y * sw + x) * 3 + i] = bar.rgba[o + i] * k;
        }
    }
}
function stoneAt(u, v) {
    const mirror = (t, n) => { t = ((t % (2 * n)) + 2 * n) % (2 * n); return t < n ? t : 2 * n - 1 - t; };
    const x = mirror(Math.floor(u), sw), y = mirror(Math.floor(v), sh), o = (y * sw + x) * 3;
    return [stone[o], stone[o + 1], stone[o + 2]];
}

// How much of the stone's light the frame's shadow leaves, d texels in from the frame's inner edge (1 past it).
const stoneMean = (() => { let a = 0; for (let i = 0; i < stone.length; i++) a += stone[i]; return a / stone.length; })();
function shadow(d) {
    if (d >= SHADOW_END - FRAME_END) return 1;
    const row = (() => { let a = 0; for (let x = sx0; x < sx1; x++) { const t = texel(x + 0.5, FRAME_END + Math.max(0, d) + 0.5); a += (t[0] + t[1] + t[2]) / 3; } return a / sw; })();
    return Math.min(1, row / stoneMean);
}
const SHADOWS = [...Array(SHADOW_END - FRAME_END + 1)].map((_, d) => shadow(d));
const shadowAt = (d) => (d <= 0 ? SHADOWS[0] : d >= SHADOWS.length - 1 ? 1 : SHADOWS[Math.floor(d)] + (SHADOWS[Math.floor(d) + 1] - SHADOWS[Math.floor(d)]) * (d % 1));

// ---- The plate ------------------------------------------------------------------------------------------------------

const SIZE = 512, UNIT = SIZE / 288, PIVOT = 370;
const R_OUT = 205, LO = 85, HI = 220, HUB = 47;
const FRAME_W = FRAME_END - FRAME;
const RECESS = 0.45;   // the hub's recess and the stone behind the frame's windows, as dark as the bar's slots
const rad = (deg) => deg * Math.PI / 180;
// The joints are spaced so that one falls on each corner: the arc's frame along its middle, the ends from the corner
// to the hub.
const ARC_SCALE = Math.round(rad(HI - LO) * (R_OUT - FRAME_W / 2) / JOINT) * JOINT / rad(HI - LO);
const END_SCALE = Math.round((R_OUT - HUB) / JOINT) * JOINT / (R_OUT - HUB);

function over(dst, src, a) {
    for (let i = 0; i < 3; i++) dst[i] = dst[i] * (1 - a) + src[i] * a;
    dst[3] = dst[3] + (1 - dst[3]) * a;
}

// One sample at (x, y) units from the pivot, y up: [r, g, b, a] with a from 0 to 1.
function sample(x, y) {
    const r = Math.hypot(x, y);
    let t = Math.atan2(y, x) * 180 / Math.PI;
    if (t < 0) t += 360;
    const out = [0, 0, 0, 0];
    if (r <= R_OUT && t >= LO && t <= HI) {
        // Distance in from each edge, and where along it: the arc, the end at LO, the end at HI.
        const edges = [[R_OUT - r, JOINT_X + rad(t - LO) * ARC_SCALE]];
        for (const [a, sign] of [[LO, 1], [HI, -1]]) {
            const off = rad((t - a) * sign);
            edges.push(off < Math.PI / 2 ? [r * Math.sin(off), JOINT_X + (R_OUT - r * Math.cos(off)) * END_SCALE] : [Infinity, 0]);
        }
        const near = edges.reduce((a, b) => (b[0] < a[0] ? b : a));
        const s = stoneAt(rad(t) * r, r);
        let light = 1;
        for (const [d] of edges) light *= shadowAt(d - FRAME_W);
        if (near[0] < FRAME_W) light = RECESS;   // behind the frame's windows
        over(out, s.map((c) => c * light), 1);
        if (near[0] < FRAME_W) {
            const f = texel(near[1], FRAME + near[0]);
            over(out, f, f[3] / 255);
        }
    }
    if (r < HUB) {
        // The hub: the frame's inner rail (its last 3 rows, once round) about a recess as dark as the slots, in the
        // rail's shadow.
        const d = HUB - r, RAIL = 3;
        if (d >= RAIL) over(out, stoneAt(rad(t) * r, r).map((c) => c * RECESS * shadowAt(d - RAIL)), 1);
        else {
            const f = texel(JOINT_X + JOINT / 2 + t / 360 * 256, FRAME_END - RAIL + d);
            over(out, f, f[3] / 255);
        }
    }
    return out;
}

// 3 x 3 samples a pixel; premultiplied while averaging.
const SS = 3;
const img = new Float32Array(SIZE * SIZE * 4);
for (let py = 0; py < SIZE; py++) {
    for (let px = 0; px < SIZE; px++) {
        const acc = [0, 0, 0, 0];
        for (let j = 0; j < SS; j++) {
            for (let i = 0; i < SS; i++) {
                const s = sample((px + (i + 0.5) / SS - PIVOT) / UNIT, (PIVOT - py - (j + 0.5) / SS) / UNIT);
                for (let k = 0; k < 3; k++) acc[k] += s[k] * s[3];
                acc[3] += s[3];
            }
        }
        const o = (py * SIZE + px) * 4;
        for (let k = 0; k < 3; k++) img[o + k] = acc[3] ? acc[k] / acc[3] : 0;
        img[o + 3] = acc[3] / (SS * SS);
    }
}

// A soft drop shadow under it all, down and to the right as the bar's own.
function blur(a, radius) {   // three box passes each way
    const tmp = new Float32Array(a.length);
    for (let pass = 0; pass < 3; pass++) {
        for (const [src, dst, horizontal] of [[a, tmp, true], [tmp, a, false]]) {
            for (let line = 0; line < SIZE; line++) {
                let sum = 0;
                const at = (k) => (horizontal ? line * SIZE + k : k * SIZE + line);
                for (let k = -radius; k <= radius; k++) sum += src[at(Math.max(0, Math.min(SIZE - 1, k)))];
                for (let k = 0; k < SIZE; k++) {
                    dst[at(k)] = sum / (2 * radius + 1);
                    sum += src[at(Math.min(SIZE - 1, k + radius + 1))] - src[at(Math.max(0, k - radius))];
                }
            }
        }
    }
    return a;
}
const DROP = [3, 5], DROP_ALPHA = 0.7;
const drop = new Float32Array(SIZE * SIZE);
for (let y = 0; y < SIZE; y++) {
    for (let x = 0; x < SIZE; x++) {
        const sx = x - DROP[0], sy = y - DROP[1];
        if (sx >= 0 && sy >= 0) drop[y * SIZE + x] = img[(sy * SIZE + sx) * 4 + 3];
    }
}
blur(drop, 4);

// RLE TGA, 32-bit, bottom-left origin (as make-touch-art.py's PIL writes them); runs stay within a row.
const header = Buffer.alloc(18);
header[2] = 10;
header.writeUInt16LE(SIZE, 12);
header.writeUInt16LE(SIZE, 14);
header[16] = 32;
header[17] = 8;
const body = [];
for (let py = SIZE - 1; py >= 0; py--) {
    const row = [];
    for (let px = 0; px < SIZE; px++) {
        const o = (py * SIZE + px) * 4, a = img[o + 3], sa = drop[py * SIZE + px] * DROP_ALPHA * (1 - a);
        const outA = a + sa;
        const c = [0, 1, 2].map((k) => (outA ? Math.round(img[o + k] * a / outA) : 0));
        row.push([c[2], c[1], c[0], Math.round(outA * 255)]);   // BGRA
    }
    for (let i = 0; i < SIZE;) {
        let run = 1;
        while (i + run < SIZE && run < 128 && row[i + run].every((v, k) => v === row[i][k])) run++;
        if (run > 1) {
            body.push(Buffer.from([0x80 | (run - 1), ...row[i]]));
            i += run;
            continue;
        }
        let raw = 1;
        while (i + raw < SIZE && raw < 128 && !row[i + raw].every((v, k) => v === row[i + raw - 1][k])) raw++;
        if (i + raw < SIZE && raw > 1) raw--;   // leave the start of the next run to it
        body.push(Buffer.from([raw - 1, ...row.slice(i, i + raw).flat()]));
        i += raw;
    }
}
const outDir = join(gameDir, "Interface", "TouchUI");
mkdirSync(outDir, { recursive: true });
writeFileSync(join(outDir, "ArcPlate.tga"), Buffer.concat([header, ...body]));
console.log(`make-arc-plate: ${join(outDir, "ArcPlate.tga")}`);
