// Pulls the client's own loading-screen art out of the game data, so the page's loading screen (web/index.html) looks
// like the client's (LoadingScreen.cpp). The textures are Blizzard's, so they are extracted on the server, never
// committed:
//
//   node Tools/Bridge/extract-art.mjs [--game bin/Win] [--out bin/Web]
//
// Writes loading-bar-border.png, loading-bar-fill.png, loading-<n>.png and loading-art.json (the screens found and
// whether each is the 16:10 "wide" painting).

import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { decodeBlp, openGameData, png } from "./game-data.mjs";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const argv = process.argv.slice(2);
const arg = (name, fallback) => (argv.indexOf(`--${name}`) >= 0 ? argv[argv.indexOf(`--${name}`) + 1] : fallback);
const gameDir = resolve(root, arg("game", "bin/Win"));
const outDir = resolve(root, arg("out", "bin/Web"));

// The continent paintings LoadingScreens.dbc points at; whichever exist are used ("Wide" ones are 16:10).
const SCREENS = ["LoadScreenNorthrend", "LoadScreenKalimdor", "LoadScreenEasternKingdom", "LoadScreenOutland"];

const readFile = await openGameData(gameDir);

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
// The font the client draws the loading-screen tip in (LoadingScreen.cpp, enUS).
const font = readFile("Fonts\\FRIZQT__.TTF");
if (font) writeFileSync(join(outDir, "loading-font.ttf"), font);
console.log(`extract-art: ${screens.length} loading screens, the loading bar${font ? " and the tip font" : ""} written to ${outDir}`);

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
