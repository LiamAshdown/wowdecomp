// Linked with --pre-js. Wow.js also runs inside every pthread worker, where uncaught errors otherwise reach the page
// as a bare message without a stack.
if (typeof WorkerGlobalScope !== "undefined") {
    self.addEventListener("error", (event) => {
        console.error("[worker] " + ((event.error && event.error.stack) || event.message));
    });
}

// The page's memory ceiling (window.wowMemoryMB, index.html) in place of the link-time 4 GB. A shared memory reserves
// address space for its whole maximum, which phones refuse, so a refused maximum is halved until it fits. A smaller
// maximum still matches the import Wow.wasm declares; growing past it fails like running out of memory.
if (typeof window !== "undefined" && window.WebAssembly) {
    const Memory = WebAssembly.Memory;
    WebAssembly.Memory = function (descriptor) {
        const limits = Object.assign({}, descriptor);
        if (limits.shared && window.wowMemoryMB) limits.maximum = Math.min(limits.maximum, Math.max(limits.initial, window.wowMemoryMB * 16));
        for (;;) {
            try {
                const memory = new Memory(limits);
                console.log(`[memory] ${limits.initial / 16} MB, at most ${limits.maximum / 16} MB`);
                return memory;
            } catch (e) {
                if (!limits.shared || limits.maximum <= limits.initial) throw e;
                limits.maximum = Math.max(limits.initial, limits.maximum >> 1);
            }
        }
    };
    WebAssembly.Memory.prototype = Memory.prototype;
}
