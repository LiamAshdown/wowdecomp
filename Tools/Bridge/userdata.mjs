// Path rules for the client's persisted writes (PUT/DELETE /userdata/<path>, see bridge.mjs).
// Returns the canonical relative path ('/'-separated, top-level directory in its canonical case) or null.

// Top-level directories the client may persist. Logs/ is deliberately absent (churn, no value across reloads);
// Interface/ is absent so a page can never plant AddOns that the native client would also load.
export const ALLOWED_DIRS = ["WTF", "Cache", "Screenshots"];
export const MAX_FILE_BYTES = 64 * 1024 * 1024;

const BAD_CHARS = /[\x00-\x1f<>:"\\|?*]/;
const DEVICE_NAMES = /^(con|prn|aux|nul|com\d|lpt\d)(\..*)?$/i;

export function userPath(rel) {
    if (typeof rel !== "string" || rel.length === 0 || rel.length > 512 || rel.startsWith("/")) return null;
    const parts = rel.split("/");
    if (parts.length < 2) return null;
    for (const p of parts) {
        if (p === "" || p === "." || p === ".." || p.length > 255 || BAD_CHARS.test(p)) return null;
        if (p.endsWith(".") || p.endsWith(" ") || DEVICE_NAMES.test(p)) return null;
    }
    const top = ALLOWED_DIRS.find((d) => d.toLowerCase() === parts[0].toLowerCase());
    if (!top) return null;
    parts[0] = top;
    return parts.join("/");
}
