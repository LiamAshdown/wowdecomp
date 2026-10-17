// Linked with --pre-js. Wow.js also runs inside every pthread worker, where uncaught errors otherwise reach the page
// as a bare message without a stack.
if (typeof WorkerGlobalScope !== "undefined") {
    self.addEventListener("error", (event) => {
        console.error("[worker] " + ((event.error && event.error.stack) || event.message));
    });
}
