// node Tools/Bridge/addons.test.mjs
import assert from "node:assert/strict";
import { catalogText, installedNames, libraryFile, loadLibrary, manifestLines, parseToc, validName } from "./addons.mjs";
import { parseSources } from "./fetch-addons.mjs";

for (const name of ["Questie", "DBM-Core", "Ace3", "_NPCScan", "Foo.Bar"]) assert.ok(validName(name), name);
for (const name of ["", ".", "..", ".hidden", "-x", "Blizzard_AuctionUI", "blizzard_x", "a/b", "a\\b", "a b", "x".repeat(65)]) {
    assert.ok(!validName(name), JSON.stringify(name));
}

assert.deepEqual(parseToc("﻿## Interface: 30300\r\n## Title: |cff00ff00Foo|r\n## Notes: Does\tthings: well\nFoo.lua\n# ## Title: no"),
    { interface: "30300", title: "|cff00ff00Foo|r", notes: "Does things: well" });

// A fake library folder: name -> files (rel -> [size, text]).
const disk = {
    Foo: { "Foo.toc": [40, "## Title: Foo\n## Version: 1.2\n## Author: Me"], "Foo.lua": [100], "Libs/Lib.lua": [60] },
    Bar: { "bar.TOC": [10, "## Notes: lower-case toc"] },
    NoToc: { "NoToc.lua": [1] },
    Shipped: { "Shipped.toc": [1, ""] },
    "..": { "...toc": [1, ""] },
    Blizzard_Evil: { "Blizzard_Evil.toc": [1, ""] },
    // A package: the main addon's folder gives the title; folders without their own .toc and loose files are left out.
    "DBM-Core": {
        "LICENSE.txt": [5], "DBM-Core/DBM-Core.toc": [20, "## Title: DBM Core"], "DBM-Core/DBM-Core.lua": [300],
        "DBM-GUI/DBM-GUI.toc": [10, ""], "DBM-Party/DBM-Party.toc": [10, ""], "DBM-Party/Boss.lua": [50], "Docs/readme.md": [9],
    },
    // A package whose DBM-GUI folder the one above already claimed.
    Clash: { "Clash/Clash.toc": [1, ""], "DBM-GUI/DBM-GUI.toc": [1, ""] },
    // A package folder named after a shipped addon.
    Sneaky: { "Sneaky/Sneaky.toc": [1, ""], "Shipped/Shipped.toc": [1, ""] },
};
const walk = (dir) => new Map(Object.entries(disk[dir.split("/").pop()]).map(([rel, [size]]) => [rel, size]));
const readText = (file) => {
    const [, name, rel] = /^lib\/([^/]+)\/(.+)$/.exec(file);
    return disk[name][rel][1];
};
const library = loadLibrary("lib", Object.keys(disk), walk, readText, new Set(["shipped"]));

assert.deepEqual([...library.keys()].sort(), ["Bar", "DBM-Core", "Foo"]);
assert.deepEqual({ ...library.get("Foo"), folders: undefined }, { title: "Foo", notes: "", author: "Me", version: "1.2", bytes: 200, folders: undefined });
assert.equal(library.get("Bar").title, "Bar");
assert.deepEqual([...library.get("DBM-Core").folders.keys()], ["DBM-Core", "DBM-GUI", "DBM-Party"]);
assert.equal(library.get("DBM-Core").title, "DBM Core");
assert.equal(library.get("DBM-Core").bytes, 390);

const dbm = new Set(["DBM-Core"]);
assert.deepEqual(manifestLines(library, dbm), [
    "20\tInterface/AddOns/DBM-Core/DBM-Core.toc", "300\tInterface/AddOns/DBM-Core/DBM-Core.lua",
    "10\tInterface/AddOns/DBM-GUI/DBM-GUI.toc", "10\tInterface/AddOns/DBM-Party/DBM-Party.toc",
    "50\tInterface/AddOns/DBM-Party/Boss.lua",
]);
assert.deepEqual(libraryFile(library, dbm, "Interface/AddOns/DBM-Party/Boss.lua"), { path: "DBM-Core/DBM-Party/Boss.lua", size: 50 });
assert.deepEqual(libraryFile(library, new Set(["Foo"]), "Interface/AddOns/Foo/Libs/Lib.lua"), { path: "Foo/Libs/Lib.lua", size: 60 });
// Only installed entries, only listed files, never a loose package file.
for (const path of ["Interface/AddOns/Foo/Foo.lua", "Interface/AddOns/DBM-Party/missing.lua", "Interface/AddOns/Docs/readme.md",
    "Interface/AddOns/DBM-Core/../LICENSE.txt", "Data/common.MPQ"]) {
    assert.equal(libraryFile(library, dbm, path), null, path);
}

// Case-insensitive, library names only, once each; unknown names and blank lines are ignored.
const installed = installedNames("foo\r\n\nNOPE\nFOO\n  bar  \n../x\n", library);
assert.deepEqual([...installed], ["Foo", "Bar"]);
assert.deepEqual([...installedNames(undefined, library)], []);

assert.deepEqual(manifestLines(library, installed), [
    "40\tInterface/AddOns/Foo/Foo.toc", "100\tInterface/AddOns/Foo/Foo.lua", "60\tInterface/AddOns/Foo/Libs/Lib.lua",
    "10\tInterface/AddOns/Bar/bar.TOC",
]);
assert.equal(catalogText(library, new Set(["Foo"])),
    "Bar\t0\t10\tBar\t\t\tlower-case toc\nDBM-Core\t0\t390\tDBM Core\t\t\t\nFoo\t1\t200\tFoo\t1.2\tMe\t\n");

assert.deepEqual(parseSources("# comment\n\nFoo https://github.com/a/Foo - \nDBM-Core https://github.com/a/DBM v1 DBM-Core DBM-* # zones\n"), [
    { name: "Foo", url: "https://github.com/a/Foo", ref: null, paths: [], renames: [] },
    { name: "DBM-Core", url: "https://github.com/a/DBM", ref: "v1", paths: ["DBM-Core", "DBM-*"], renames: [] },
]);
assert.deepEqual(parseSources("NP https://x/NP - NP-3.3.5.toc=NP.toc")[0].renames, [["NP-3.3.5.toc", "NP.toc"]]);
for (const bad of ["Foo", "Foo http://x/y -", "../x https://x/y -", "Foo https://x/y", "Foo https://x/y - a=../b", "Foo https://x/y - a=",
    "Foo https://x/y - a=b=c", "Foo https://x/y - =b"]) {
    assert.throws(() => parseSources(bad), Error, bad);
}

console.log("addon library ok");
