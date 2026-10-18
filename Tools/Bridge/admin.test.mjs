// node --test Tools/Bridge
import assert from "node:assert/strict";
import test from "node:test";
import { mapPoint, pageEvent } from "./admin.mjs";

test("page events: only the known events and details, ms only for loaded and abandoned loads", () => {
    assert.deepEqual(pageEvent({ event: "visit", detail: "firefox" }), { event: "visit", detail: "firefox", ms: 0 });
    assert.deepEqual(pageEvent({ event: "loaded", detail: "", ms: 31234.6 }), { event: "loaded", detail: "", ms: 31235 });
    assert.deepEqual(pageEvent({ event: "load_failed", detail: "webgl2", ms: 5 }), { event: "load_failed", detail: "webgl2", ms: 0 });
    assert.equal(pageEvent({ event: "loaded", ms: 10_000_000 }).ms, 0);
    assert.deepEqual(pageEvent({ event: "load_abandoned", detail: "download", ms: 8000 }), { event: "load_abandoned", detail: "download", ms: 8000 });
    assert.deepEqual(pageEvent({ event: "load_slow", detail: "", ms: 180000 }), { event: "load_slow", detail: "", ms: 0 });
    assert.equal(pageEvent({ event: "load_abandoned", detail: "login" }), null);
    assert.equal(pageEvent({ event: "visit", detail: "netscape" }), null);
    assert.equal(pageEvent({ event: "account_created" }), null);   // server-side only
    assert.equal(pageEvent({ event: "visit", detail: "x".repeat(5000) }), null);
    assert.equal(pageEvent(null), null);
});

test("map points: world position to the continent map, null off it", () => {
    const northrend = { left: 9217.15, right: -8534.25, top: 10593.38, bottom: -1240.89 };
    const [fx, fy] = mapPoint(northrend, 5804, 624);   // Dalaran
    assert.ok(Math.abs(fx - 0.484) < 0.01 && Math.abs(fy - 0.405) < 0.01, `${fx} ${fy}`);
    assert.equal(mapPoint(northrend, 20000, 0), null);
});
