// Web Audio side of the FMOD browser output (fmod_os_output.cpp). Every entry point is proxied synchronously to
// the main browser thread, which owns the AudioContext. One context is shared by every FMOD system; each started
// output gets its own AudioWorkletNode reading its ring buffer straight out of wasm shared memory.
//
// window.wowAudio.stats() returns, per output: frames written by the mixer, frames consumed by the worklet,
// underrun quanta and the peak |sample| of the last ~250 ms. Handy because nobody can hear a test run.

addToLibrary({
    $FMODWA: {
        ctx: null,
        ready: null,
        outputs: {},

        // Runs inside AudioWorkletGlobalScope; shipped as source text through a Blob URL.
        worklet: function () {
            class WowFmodOutput extends AudioWorkletProcessor {
                constructor(options) {
                    super();
                    const p = options.processorOptions;
                    this.i32 = new Int32Array(p.buffer);
                    this.f32 = new Float32Array(p.buffer);
                    this.header = p.header;
                    this.data = p.data;
                    this.capacity = p.capacity;
                    this.channels = p.channels;
                    this.read = 0;
                    this.pos = 0;
                    this.underruns = 0;
                    this.peak = 0;
                    this.tick = 0;
                    this.alive = true;
                    this.port.onmessage = () => { this.alive = false; };
                }

                process(inputs, outputs) {
                    const out = outputs[0];
                    const left = out[0];
                    const right = out.length > 1 ? out[1] : null;
                    const n = left.length;
                    const written = Atomics.load(this.i32, this.header);

                    if (((written - this.read) | 0) < n) {
                        this.underruns++;    // outputs start zeroed: silence, and the read cursor holds
                    } else {
                        const f32 = this.f32, ch = this.channels, cap = this.capacity, base = this.data;
                        let pos = this.pos, peak = this.peak;
                        for (let i = 0; i < n; i++) {
                            const idx = base + pos * ch;
                            const l = f32[idx];
                            const r = ch > 1 ? f32[idx + 1] : l;
                            left[i] = l;
                            if (right) right[i] = r;
                            const a = Math.max(Math.abs(l), Math.abs(r));
                            if (a > peak) peak = a;
                            if (++pos === cap) pos = 0;
                        }
                        this.pos = pos;
                        this.peak = peak;
                        this.read = (this.read + n) | 0;
                        Atomics.store(this.i32, this.header + 1, this.read);
                    }

                    if ((this.tick += n) >= sampleRate / 4) {
                        this.port.postMessage({ underruns: this.underruns, peak: this.peak });
                        this.tick = 0;
                        this.peak = 0;
                    }
                    return this.alive;
                }
            }
            registerProcessor("wow-fmod-output", WowFmodOutput);
        },

        resume: function () {
            if (FMODWA.ctx && FMODWA.ctx.state === "suspended") FMODWA.ctx.resume();
        },

        stats: function () {
            const res = { state: FMODWA.ctx ? FMODWA.ctx.state : "none", sampleRate: FMODWA.ctx ? FMODWA.ctx.sampleRate : 0, outputs: [] };
            const i32 = new Int32Array(wasmMemory.buffer);
            for (const key in FMODWA.outputs) {
                const o = FMODWA.outputs[key];
                res.outputs.push({ ring: o.ring, channels: o.channels, written: Atomics.load(i32, o.ring >> 2) >>> 0,
                    consumed: Atomics.load(i32, (o.ring >> 2) + 1) >>> 0, underruns: o.underruns, peak: o.peak });
            }
            return res;
        },
    },

    fmod_webaudio_available__deps: ["$FMODWA"],
    fmod_webaudio_available__proxy: "sync",
    fmod_webaudio_available__sig: "i",
    fmod_webaudio_available: function () {
        return (typeof AudioContext !== "undefined" && typeof AudioWorkletNode !== "undefined" && typeof SharedArrayBuffer !== "undefined") ? 1 : 0;
    },

    // Creates (once) the shared AudioContext and starts loading the worklet; returns its sample rate, 0 on failure.
    fmod_webaudio_open__deps: ["$FMODWA"],
    fmod_webaudio_open__proxy: "sync",
    fmod_webaudio_open__sig: "ii",
    fmod_webaudio_open: function (rate) {
        if (!FMODWA.ctx) {
            try {
                FMODWA.ctx = new AudioContext({ sampleRate: rate, latencyHint: "interactive" });
            } catch (e) {
                try { FMODWA.ctx = new AudioContext({ latencyHint: "interactive" }); } catch (e2) { return 0; }
            }
            const url = URL.createObjectURL(new Blob(["(" + FMODWA.worklet.toString() + ")();"], { type: "text/javascript" }));
            FMODWA.ready = FMODWA.ctx.audioWorklet.addModule(url);
            FMODWA.ready.catch((e) => err("fmod webaudio: worklet failed to load: " + e));
            // Autoplay policy: the context starts suspended until the page sees a user gesture.
            for (const type of ["pointerdown", "keydown", "touchend"]) window.addEventListener(type, FMODWA.resume, true);
            globalThis.wowAudio = FMODWA;
        }
        return FMODWA.ctx.sampleRate;
    },

    fmod_webaudio_start__deps: ["$FMODWA"],
    fmod_webaudio_start__proxy: "sync",
    fmod_webaudio_start__sig: "vpii",
    fmod_webaudio_start: function (ring, capacity, channels) {
        const o = { ring: ring, channels: channels, node: null, stopped: false, underruns: 0, peak: 0 };
        FMODWA.outputs[ring] = o;
        FMODWA.ready.then(() => {
            if (o.stopped) return;
            o.node = new AudioWorkletNode(FMODWA.ctx, "wow-fmod-output", {
                numberOfInputs: 0,
                numberOfOutputs: 1,
                outputChannelCount: [2],
                processorOptions: { buffer: wasmMemory.buffer, header: ring >> 2, data: (ring >> 2) + 4, capacity: capacity, channels: channels },
            });
            o.node.port.onmessage = (e) => { o.underruns = e.data.underruns; o.peak = e.data.peak; };
            o.node.connect(FMODWA.ctx.destination);
            FMODWA.resume();
        });
    },

    fmod_webaudio_stop__deps: ["$FMODWA"],
    fmod_webaudio_stop__proxy: "sync",
    fmod_webaudio_stop__sig: "vp",
    fmod_webaudio_stop: function (ring) {
        const o = FMODWA.outputs[ring];
        if (!o) return;
        o.stopped = true;
        if (o.node) {
            o.node.port.postMessage("stop");
            o.node.disconnect();
        }
        delete FMODWA.outputs[ring];
    },
});
