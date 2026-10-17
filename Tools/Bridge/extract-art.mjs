// Pulls the client's own loading-screen art out of the game data, so the page's loading screen (web/index.html) looks
// like the client's (LoadingScreen.cpp). The textures are Blizzard's, so they are extracted on the server, never
// committed:
//
//   node Tools/Bridge/extract-art.mjs [--game bin/Win] [--out bin/Web]
//
// Writes loading-bar-border.png, loading-bar-fill.png, loading-<n>.png and loading-art.json (the screens found and
// whether each is the 16:10 "wide" painting).

import { crc32, deflateSync } from "node:zlib";
import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

process.env.NODE_ENV = "production";   // StormJS' quiet build
const { FS, MPQ } = await import("@wowserhq/stormjs");

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const argv = process.argv.slice(2);
const arg = (name, fallback) => (argv.indexOf(`--${name}`) >= 0 ? argv[argv.indexOf(`--${name}`) + 1] : fallback);
const gameDir = resolve(root, arg("game", "bin/Win"));
const outDir = resolve(root, arg("out", "bin/Web"));

// Highest priority first, as the client mounts them.
const ARCHIVES = ["enUS/patch-enUS-3", "enUS/patch-enUS-2", "enUS/patch-enUS", "patch-3", "patch-2", "patch",
    "enUS/lichking-locale-enUS", "enUS/expansion-locale-enUS", "enUS/locale-enUS", "lichking", "expansion", "common-2",
    "common"];

// The continent paintings LoadingScreens.dbc points at; whichever exist are used ("Wide" ones are 16:10).
const SCREENS = ["LoadScreenNorthrend", "LoadScreenKalimdor", "LoadScreenEasternKingdom", "LoadScreenOutland"];

FS.mkdir("/data");
FS.mount(FS.filesystems.NODEFS, { root: join(gameDir, "Data") }, "/data");
const archives = [];
for (const name of ARCHIVES) {
    try {
        archives.push(await MPQ.open(`/data/${name}.MPQ`, "r"));
    } catch {}
}

function readFile(name) {
    for (const mpq of archives) {
        if (!mpq.hasFile(name)) continue;
        const file = mpq.openFile(name);
        const data = Buffer.from(file.read());
        file.close();
        return data;
    }
    return null;
}

// ---- BLP2 -> RGBA ------------------------------------------------------------------------------------------------

function rgb565(c) {
    return [((c >> 11) & 31) * 255 / 31 | 0, ((c >> 5) & 63) * 255 / 63 | 0, (c & 31) * 255 / 31 | 0];
}

// One 4x4 DXT colour block at `at` into out (RGBA, width w). DXT1 blocks with c0 <= c1 use 3 colours plus index 3,
// which is transparent when the texture has alpha and black otherwise.
function colorBlock(src, at, out, w, x0, y0, dxt1, transparent) {
    const c0 = src.readUInt16LE(at), c1 = src.readUInt16LE(at + 2), bits = src.readUInt32LE(at + 4);
    const p0 = rgb565(c0), p1 = rgb565(c1);
    const pal = [p0, p1];
    if (c0 > c1 || !dxt1) {
        pal.push(p0.map((v, i) => (2 * v + p1[i]) / 3 | 0), p0.map((v, i) => (v + 2 * p1[i]) / 3 | 0));
    } else {
        pal.push(p0.map((v, i) => (v + p1[i]) / 2 | 0), transparent ? null : [0, 0, 0]);
    }
    for (let i = 0; i < 16; i++) {
        const x = x0 + (i & 3), y = y0 + (i >> 2);
        if (x >= w || y * w + x >= out.length / 4) continue;
        const color = pal[(bits >>> (2 * i)) & 3];
        const o = (y * w + x) * 4;
        if (color) [out[o], out[o + 1], out[o + 2]] = color;
        else out[o + 3] = 0;
    }
}

function decodeBlp(blp) {
    if (blp.toString("latin1", 0, 4) !== "BLP2") throw new Error("not a BLP2 texture");
    const compression = blp[8], alphaDepth = blp[9], alphaType = blp[10];
    const w = blp.readUInt32LE(12), h = blp.readUInt32LE(16);
    const offset = blp.readUInt32LE(20), size = blp.readUInt32LE(84);
    const src = blp.subarray(offset, offset + size);
    const out = Buffer.alloc(w * h * 4, 255);

    if (compression === 1) {
        // Palettized: BGRA palette, w*h indices, then alphaDepth-bit alpha.
        for (let i = 0; i < w * h; i++) {
            const p = 148 + src[i] * 4;
            out[i * 4] = blp[p + 2]; out[i * 4 + 1] = blp[p + 1]; out[i * 4 + 2] = blp[p];
            if (alphaDepth === 8) out[i * 4 + 3] = src[w * h + i];
            else if (alphaDepth === 4) out[i * 4 + 3] = ((src[w * h + (i >> 1)] >> ((i & 1) * 4)) & 15) * 17;
            else if (alphaDepth === 1) out[i * 4 + 3] = (src[w * h + (i >> 3)] >> (i & 7)) & 1 ? 255 : 0;
        }
    } else if (compression === 2) {
        // DXT: alphaType 0 = DXT1, 1 = DXT3, 7 = DXT5.
        const dxt = alphaType === 7 ? 5 : alphaType === 1 && alphaDepth ? 3 : 1;
        const blockBytes = dxt === 1 ? 8 : 16;
        let at = 0;
        for (let by = 0; by < h; by += 4) {
            for (let bx = 0; bx < w; bx += 4, at += blockBytes) {
                if (dxt === 1) {
                    colorBlock(src, at, out, w, bx, by, true, alphaDepth > 0);
                    continue;
                }
                colorBlock(src, at + 8, out, w, bx, by, false, false);
                for (let i = 0; i < 16; i++) {
                    const x = bx + (i & 3), y = by + (i >> 2);
                    if (x >= w || y >= h) continue;
                    let a;
                    if (dxt === 3) {
                        a = ((src[at + (i >> 1)] >> ((i & 1) * 4)) & 15) * 17;
                    } else {
                        const a0 = src[at], a1 = src[at + 1];
                        const bits = src.readUIntLE(at + 2, 6);
                        const code = Math.floor(bits / 2 ** (3 * i)) & 7;
                        a = code === 0 ? a0 : code === 1 ? a1
                            : a0 > a1 ? ((8 - code) * a0 + (code - 1) * a1) / 7 | 0
                            : code === 6 ? 0 : code === 7 ? 255 : ((6 - code) * a0 + (code - 1) * a1) / 5 | 0;
                    }
                    out[(y * w + x) * 4 + 3] = a;
                }
            }
        }
    } else if (compression === 3) {
        for (let i = 0; i < w * h; i++) {
            out[i * 4] = src[i * 4 + 2]; out[i * 4 + 1] = src[i * 4 + 1]; out[i * 4 + 2] = src[i * 4]; out[i * 4 + 3] = src[i * 4 + 3];
        }
    } else {
        throw new Error(`unknown BLP compression ${compression}`);
    }
    return { w, h, rgba: out };
}

// ---- RGBA -> PNG ---------------------------------------------------------------------------------------------------

function png({ w, h, rgba }) {
    const chunk = (type, data) => {
        const head = Buffer.alloc(8);
        head.writeUInt32BE(data.length);
        head.write(type, 4, "latin1");
        const crc = Buffer.alloc(4);
        crc.writeUInt32BE(crc32(Buffer.concat([head.subarray(4), data])));
        return Buffer.concat([head, data, crc]);
    };
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(w);
    ihdr.writeUInt32BE(h, 4);
    ihdr.set([8, 6, 0, 0, 0], 8);   // 8-bit RGBA
    const rows = Buffer.alloc(h * (w * 4 + 1));
    for (let y = 0; y < h; y++) rgba.copy(rows, y * (w * 4 + 1) + 1, y * w * 4, (y + 1) * w * 4);
    return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", deflateSync(rows)),
        chunk("IEND", Buffer.alloc(0))]);
}

// ---- Extraction ----------------------------------------------------------------------------------------------------

mkdirSync(outDir, { recursive: true });

function extract(texture, outName) {
    const blp = readFile(`${texture}.blp`);
    if (!blp) return false;
    writeFileSync(join(outDir, outName), png(decodeBlp(blp)));
    return true;
}

if (!extract("Interface\\Glues\\LoadingBar\\Loading-BarBorder", "loading-bar-border.png")
    || !extract("Interface\\Glues\\LoadingBar\\Loading-BarFill", "loading-bar-fill.png")) {
    console.error("extract-art: the loading bar textures are not in the game data");
    process.exit(1);
}

const screens = [];
for (const name of SCREENS) {
    for (const wide of [true, false]) {
        const file = `loading-${screens.length}.png`;
        if (extract(`Interface\\Glues\\LoadingScreens\\${name}${wide ? "Wide" : ""}`, file)) {
            screens.push({ file, wide });
            break;
        }
    }
}
writeFileSync(join(outDir, "loading-art.json"), JSON.stringify({ screens }));
console.log(`extract-art: ${screens.length} loading screens and the loading bar written to ${outDir}`);

// ---- World maps (the admin page's live map, admin.mjs) -------------------------------------------------------------
// Each continent's painting (12 256 x 256 tiles, 1002 x 668 of them used, as the client's WorldMapFrame), its world
// bounds from WorldMapArea.dbc, and every area's name from AreaTable.dbc for the zone tables.

function dbc(name) {
    const d = readFile(`DBFilesClient\\${name}.dbc`);
    if (!d || d.toString("latin1", 0, 4) !== "WDBC") return [];
    const rows = d.readUInt32LE(4), size = d.readUInt32LE(12), strings = 20 + rows * size;
    return [...Array(rows)].map((_, r) => ({
        int: (field) => d.readInt32LE(20 + r * size + field * 4),
        float: (field) => d.readFloatLE(20 + r * size + field * 4),
        string: (field) => {
            const at = strings + d.readUInt32LE(20 + r * size + field * 4);
            return d.toString("utf8", at, d.indexOf(0, at));
        },
    }));
}

const continents = [];
// WorldMapArea: ID, MapID, AreaID, AreaName, LocLeft, LocRight, LocTop, LocBottom, ...; AreaID 0 is a continent.
for (const row of dbc("WorldMapArea")) {
    const map = row.int(1), name = row.string(3);
    if (row.int(2) !== 0 || ![0, 1, 530, 571].includes(map)) continue;
    const out = { w: 1002, h: 668, rgba: Buffer.alloc(1002 * 668 * 4) };
    for (let tile = 0; tile < 12; tile++) {
        const blp = readFile(`Interface\\WorldMap\\${name}\\${name}${tile + 1}.blp`);
        if (!blp) continue;
        const t = decodeBlp(blp), x0 = (tile % 4) * 256, y0 = Math.floor(tile / 4) * 256;
        for (let y = 0; y < t.h && y0 + y < out.h; y++) {
            const width = Math.min(t.w, out.w - x0);
            if (width > 0) t.rgba.copy(out.rgba, ((y0 + y) * out.w + x0) * 4, y * t.w * 4, (y * t.w + width) * 4);
        }
    }
    const file = `worldmap-${name.toLowerCase()}.png`;
    writeFileSync(join(outDir, file), png(out));
    continents.push({ name, map, file, left: row.float(4), right: row.float(5), top: row.float(6), bottom: row.float(7) });
}
// AreaTable: ID, ContinentID, ParentAreaID, ..., AreaName_lang (enUS) at 11.
const zones = Object.fromEntries(dbc("AreaTable").map((row) => [row.int(0), row.string(11)]));
writeFileSync(join(outDir, "worldmap.json"), JSON.stringify({ continents, zones }));
console.log(`extract-art: ${continents.length} world maps and ${Object.keys(zones).length} area names written to ${outDir}`);
