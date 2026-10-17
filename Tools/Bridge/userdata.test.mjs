// node Tools/Bridge/userdata.test.mjs
import assert from "node:assert/strict";
import { userPath } from "./userdata.mjs";

const ok = {
    "WTF/Config.wtf": "WTF/Config.wtf",
    "wtf/Account/ACCT/SavedVariables/Foo.lua": "WTF/Account/ACCT/SavedVariables/Foo.lua",
    "CACHE/WDB/enUS/itemcache.wdb": "Cache/WDB/enUS/itemcache.wdb",
    "WTF/Account/ACCT#1/macros-cache.txt": "WTF/Account/ACCT#1/macros-cache.txt",
};
for (const [input, want] of Object.entries(ok)) assert.equal(userPath(input), want, input);

const bad = [
    "", "WTF", "WTF/", "/WTF/Config.wtf", "../WTF/Config.wtf", "WTF/../Config.wtf", "WTF/../../etc/passwd", "WTF/./a",
    "WTF//a", "WTF\\Config.wtf", "WTF/..\\x", "C:/WTF/a", "WTF/C:/a", "WTF/a:stream", "WTF/a\0b", "WTF/con", "WTF/NUL.txt",
    "WTF/a.", "WTF/a ", "Logs/Wow.log", "Interface/AddOns/Evil/Evil.toc", "Data/patch.MPQ", "WTFX/a", "WTF/" + "a".repeat(256),
    "WTF/a?b", "WTF/a*b",
];
for (const input of bad) assert.equal(userPath(input), null, JSON.stringify(input));
assert.equal(userPath(undefined), null);

console.log("userdata paths ok");
