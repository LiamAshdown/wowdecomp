// Fills the AddOn Library folder (bridge --addons, see addons.mjs) from the sources in addons.txt: a shallow git clone per
// entry, laid out the way addons.mjs reads it. An entry is replaced only once its new copy is complete.
//
//   node Tools/Bridge/fetch-addons.mjs [--out Tools/Bridge/addons] [--only Name,Name]
//
// Restart the bridge afterwards; it reads the library at startup.

import { execFileSync } from "node:child_process";
import { cpSync, existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, renameSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { validName } from "./addons.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const arg = (name, fallback) => (argv.includes(`--${name}`) ? argv[argv.indexOf(`--${name}`) + 1] : fallback);
const out = resolve(here, "../..", arg("out", "Tools/Bridge/addons"));
const only = arg("only") ? new Set(arg("only").split(",")) : null;

// "<entry> <git url> <branch|tag|-> [path ...] [from=to ...]" lines; see addons.txt.
export function parseSources(text) {
    const sources = [];
    for (const raw of text.split(/\r?\n/)) {
        const line = raw.replace(/#.*/, "").trim();
        if (!line) continue;
        const [name, url, ref, ...rest] = line.split(/\s+/);
        const renames = rest.filter((t) => t.includes("=")).map((t) => t.split("="));
        if (!validName(name) || !/^https:\/\/\S+$/.test(url ?? "") || !ref
            || renames.some((r) => r.length !== 2 || r.some((p) => !p || p.split("/").some((part) => !part || part === "..")))) {
            throw new Error(`addons.txt: bad line: ${raw}`);
        }
        sources.push({ name, url, ref: ref === "-" ? null : ref, paths: rest.filter((t) => !t.includes("=")), renames });
    }
    return sources;
}

// The folders a path names inside the clone; a "*" in its last part matches folder names (e.g. "DBM-*").
function expand(repo, path) {
    const last = basename(path);
    if (!last.includes("*")) return [path];
    const parent = dirname(path) === "." ? "" : dirname(path);
    const pattern = new RegExp(`^${last.split("*").map((s) => s.replace(/[.+?^${}()|[\]\\]/g, "\\$&")).join(".*")}$`, "i");
    return readdirSync(join(repo, parent), { withFileTypes: true })
        .filter((e) => e.isDirectory() && pattern.test(e.name)).map((e) => (parent ? `${parent}/${e.name}` : e.name));
}

// Windows refuses a rename (EPERM/EBUSY) while something, typically a virus scanner, still has a just-written file open.
function retry(fn) {
    for (let attempt = 1; ; attempt++) {
        try {
            return fn();
        } catch (e) {
            if (attempt === 10 || !["EPERM", "EBUSY", "EACCES"].includes(e.code)) throw e;
            Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 300);
        }
    }
}

// Dot files and folders (.git, .github, .gitignore...) never ship.
const noDotFiles = (src) => !basename(src).startsWith(".");

function fetchOne({ name, url, ref, paths, renames }) {
    const repo = mkdtempSync(join(tmpdir(), "addon-"));
    const stage = join(out, `.${name}.new`);
    try {
        execFileSync("git", ["clone", "--quiet", "--depth", "1", ...(ref ? ["--branch", ref] : []), url, repo], { stdio: "inherit" });
        const commit = execFileSync("git", ["-C", repo, "rev-parse", "--short", "HEAD"]).toString().trim();
        rmSync(stage, { recursive: true, force: true });
        if (paths.length === 0) {
            // The repository is the addon.
            cpSync(repo, stage, { recursive: true, filter: noDotFiles });
        } else {
            // A package: each folder at <entry>/<folder>, plus the repository's license files beside them.
            for (const path of paths.flatMap((p) => expand(repo, p))) {
                if (!existsSync(join(repo, path))) throw new Error(`no ${path} in the repository`);
                cpSync(join(repo, path), join(stage, basename(path)), { recursive: true, filter: noDotFiles });
            }
            for (const file of readdirSync(repo)) {
                if (/^(licen[cs]e|copying)/i.test(file)) cpSync(join(repo, file), join(stage, file));
            }
        }
        // Renames inside the entry, e.g. a repository that ships one .toc per client version.
        for (const [from, to] of renames) renameSync(join(stage, from), join(stage, to));
        // The old copy moves aside first and comes back if the new one cannot take its place.
        const dest = join(out, name);
        const old = join(out, `.${name}.old`);
        rmSync(old, { recursive: true, force: true });
        if (existsSync(dest)) retry(() => renameSync(dest, old));
        try {
            retry(() => renameSync(stage, dest));
        } catch (e) {
            if (existsSync(old)) renameSync(old, dest);
            throw e;
        }
        rmSync(old, { recursive: true, force: true, maxRetries: 5 });
        console.log(`${name}: ${url} ${ref ?? "(default branch)"} @ ${commit}`);
        return true;
    } catch (e) {
        console.error(`${name}: failed: ${e.message}`);
        rmSync(stage, { recursive: true, force: true });
        return false;
    } finally {
        rmSync(repo, { recursive: true, force: true });
    }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    const sources = parseSources(readFileSync(join(here, "addons.txt"), "utf8")).filter((s) => !only || only.has(s.name));
    mkdirSync(out, { recursive: true });
    let failed = 0;
    for (const source of sources) if (!fetchOne(source)) failed++;
    console.log(`${sources.length - failed} of ${sources.length} fetched into ${out}`);
    process.exitCode = failed ? 1 : 0;
}
