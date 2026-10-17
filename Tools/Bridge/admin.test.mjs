// node --test Tools/Bridge
import assert from "node:assert/strict";
import test from "node:test";
import { pageEvent } from "./admin.mjs";

test("page events: only the known events and details, ms only for loaded", () => {
    assert.deepEqual(pageEvent({ event: "visit", detail: "firefox" }), { event: "visit", detail: "firefox", ms: 0 });
    assert.deepEqual(pageEvent({ event: "loaded", detail: "", ms: 31234.6 }), { event: "loaded", detail: "", ms: 31235 });
    assert.deepEqual(pageEvent({ event: "load_failed", detail: "webgl2", ms: 5 }), { event: "load_failed", detail: "webgl2", ms: 0 });
    assert.equal(pageEvent({ event: "loaded", ms: 10_000_000 }).ms, 0);
    assert.equal(pageEvent({ event: "visit", detail: "netscape" }), null);
    assert.equal(pageEvent({ event: "account_created" }), null);   // server-side only
    assert.equal(pageEvent({ event: "visit", detail: "x".repeat(5000) }), null);
    assert.equal(pageEvent(null), null);
});
