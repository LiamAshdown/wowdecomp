// The AddOn Library: a curated folder of addons (--addons <dir>, one folder per entry) that any browser can install from
// character select (WoW/Source/Glue/Web/WebAddOns.cpp). A browser's picks are the lines of LIST_FILE in its user-data
// overlay, written through the ordinary PUT /userdata/ path; the bridge lists the picked addons' files under
// Interface/AddOns/ in that browser's manifest, and the client finds them like any other addon. Players can only pick
// from the library: Interface/ stays out of the overlay (userdata.mjs).

export const LIST_FILE = "WTF/AddOnLibrary.txt";

// A word character first, so "." and ".." never pass; Blizzard_ names are the client's own (DeleteInterfaceFiles).
const NAME = /^\w[\w.-]{0,63}$/;
export const validName = (name) => NAME.test(name) && !/^blizzard_/i.test(name);

// "## Key: value" lines of a .toc, keys lowercased. Tabs and line breaks are dropped (the catalog is tab-separated).
export function parseToc(text) {
    const meta = {};
    for (const line of text.replace(/^﻿/, "").split(/\r?\n/)) {
        const m = /^##\s*([^:]+):(.*)$/.exec(line);
        if (m) meta[m[1].trim().toLowerCase()] = m[2].replace(/[\t\r\n]/g, " ").trim();
    }
    return meta;
}

// The file of `files` (rel -> size) that is the .toc of addon folder `name`, or undefined.
const findToc = (files, name) => [...files.keys()].find((rel) => rel.toLowerCase() === `${name.toLowerCase()}.toc`);

// The library from its folder: name -> { title, notes, author, version, bytes, folders }. An entry is one addon
// (<name>/<name>.toc) or a package of addons installed together (<name>/<name>/<name>.toc for the main one, which gives
// the title, plus every sibling folder with its own .toc, e.g. DBM-Core next to DBM-GUI and the DBM-* zone mods).
// `folders` maps each addon folder the entry installs to { prefix (its path below <name>/), files: rel -> size }.
// `walk(dir)` lists a folder's files as rel path -> size; `readText(file)` reads one. An entry without a matching .toc,
// with a folder name the client could not use, or with a folder a shipped addon (`taken`, lowercase names) or an earlier
// entry already has is left out, and said so.
export function loadLibrary(dir, entries, walk, readText, taken = new Set()) {
    const library = new Map();
    const claimed = new Set(taken);
    for (const name of entries) {
        const skip = (why) => console.log(`addons: skipping ${name} (${why})`);
        if (!validName(name)) {
            skip("not an addon name");
            continue;
        }
        const all = walk(`${dir}/${name}`);
        const folders = new Map();
        if (findToc(all, name)) {
            folders.set(name, { prefix: "", files: all });
        } else {
            const byFolder = new Map();
            for (const [rel, size] of all) {
                const slash = rel.indexOf("/");
                if (slash < 0) continue;
                const folder = rel.slice(0, slash);
                if (!byFolder.has(folder)) byFolder.set(folder, new Map());
                byFolder.get(folder).set(rel.slice(slash + 1), size);
            }
            if (byFolder.has(name) && findToc(byFolder.get(name), name)) {
                for (const [folder, files] of byFolder) {
                    if (findToc(files, folder)) folders.set(folder, { prefix: `${folder}/`, files });
                }
            }
        }
        if (!folders.size) {
            skip(`no ${name}.toc`);
            continue;
        }
        const bad = [...folders.keys()].find((folder) => !validName(folder) || claimed.has(folder.toLowerCase()));
        if (bad) {
            skip(validName(bad) ? `${bad} is already a shipped or library addon` : `${bad} is not an addon name`);
            continue;
        }
        for (const folder of folders.keys()) claimed.add(folder.toLowerCase());
        const main = folders.get(name);
        const meta = parseToc(readText(`${dir}/${name}/${main.prefix}${findToc(main.files, name)}`));
        let bytes = 0;
        for (const { files } of folders.values()) for (const size of files.values()) bytes += size;
        library.set(name, { title: meta.title || name, notes: meta.notes ?? "", author: meta.author ?? "", version: meta.version ?? "", bytes, folders });
    }
    return library;
}

// The installed names from LIST_FILE's text: library names only, in library case, once each.
export function installedNames(text, library) {
    const byLower = new Map([...library.keys()].map((name) => [name.toLowerCase(), name]));
    const installed = new Set();
    for (const line of (text ?? "").split(/\r?\n/)) {
        const name = byLower.get(line.trim().toLowerCase());
        if (name) installed.add(name);
    }
    return installed;
}

// Manifest lines ("size<TAB>path") for the installed entries' files.
export function manifestLines(library, installed) {
    const lines = [];
    for (const name of installed) {
        for (const [folder, { files }] of library.get(name).folders) {
            for (const [rel, size] of files) lines.push(`${size}\tInterface/AddOns/${folder}/${rel}`);
        }
    }
    return lines;
}

// The library file behind a manifest path ("Interface/AddOns/<folder>/<rel>") of an installed entry, as { path (below the
// library folder), size }, or null.
export function libraryFile(library, installed, path) {
    const m = /^Interface\/AddOns\/([^/]+)\/(.+)$/.exec(path);
    if (!m) return null;
    for (const name of installed) {
        const folder = library.get(name).folders.get(m[1]);
        const size = folder?.files.get(m[2]);
        if (size !== undefined) return { path: `${name}/${folder.prefix}${m[2]}`, size };
    }
    return null;
}

// GET /addons/library.txt: one "name<TAB>installed<TAB>bytes<TAB>title<TAB>version<TAB>author<TAB>notes" line per addon,
// sorted by name.
export function catalogText(library, installed) {
    return [...library.keys()].sort((a, b) => a.localeCompare(b)).map((name) => {
        const a = library.get(name);
        return [name, installed.has(name) ? 1 : 0, a.bytes, a.title, a.version, a.author, a.notes].join("\t") + "\n";
    }).join("");
}
