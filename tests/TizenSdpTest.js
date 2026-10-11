"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");

const rootDir = path.resolve(__dirname, "..");
const appJsPath = path.join(rootDir, "tizen", "app.js");

assert.ok(fs.existsSync(appJsPath), "tizen/app.js must exist");

const appContent = fs.readFileSync(appJsPath, "utf8");

// 1. Verify function definition and usage in app.js
assert.ok(
  appContent.includes("function injectVideoBandwidthSdp(sdp, bitrateKbps)"),
  "app.js must define injectVideoBandwidthSdp"
);
assert.ok(
  appContent.includes("const bandwidthCapKbps = Math.round(selectedSession.bitrateKbps * 1.25);"),
  "app.js must calculate bandwidthCapKbps with 25% headroom for packet/retransmission overhead"
);
assert.ok(
  appContent.includes("injectVideoBandwidthSdp(answer.sdp, bandwidthCapKbps)"),
  "app.js must invoke injectVideoBandwidthSdp with bandwidthCapKbps in handleOffer"
);
assert.ok(
  appContent.includes("WebRTC video bandwidth cap (b=AS) negotiated in answer"),
  "app.js must log bandwidth cap negotiation"
);

// 2. Extract function from app.js to test its exact logic
const fnMatch = appContent.match(/function injectVideoBandwidthSdp\([\s\S]*?\n\}/);
assert.ok(fnMatch, "injectVideoBandwidthSdp function body must match in app.js");
const injectVideoBandwidthSdp = new Function("return " + fnMatch[0])();

// Test 1: Empty or invalid input
assert.strictEqual(injectVideoBandwidthSdp("", 50000), "");
assert.strictEqual(injectVideoBandwidthSdp(null, 50000), null);
assert.strictEqual(injectVideoBandwidthSdp("v=0\r\n", 0), "v=0\r\n");
assert.strictEqual(injectVideoBandwidthSdp("v=0\r\n", -1), "v=0\r\n");

// Test 2: Standard SDP without b=AS (inserts after c= line in video section)
const sampleSdp =
  "v=0\r\n" +
  "o=- 12345 2 IN IP4 127.0.0.1\r\n" +
  "s=-\r\n" +
  "t=0 0\r\n" +
  "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n" +
  "c=IN IP4 0.0.0.0\r\n" +
  "a=rtcp-mux\r\n" +
  "a=rtpmap:96 AV1/90000\r\n" +
  "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n";

const injectedSdp = injectVideoBandwidthSdp(sampleSdp, 70000);
assert.ok(
  injectedSdp.includes("m=video 9 UDP/TLS/RTP/SAVPF 96\r\nc=IN IP4 0.0.0.0\r\nb=AS:70000\r\n"),
  "b=AS:70000 must be inserted after video c= line"
);

// Verify audio section does not have b=AS:70000
const audioPart = injectedSdp.substring(injectedSdp.indexOf("m=audio"));
assert.ok(!audioPart.includes("b=AS:70000"), "Audio section must not receive video bandwidth cap");

// Test 3: Replacing existing b=AS in SDP
const sdpWithExistingBas =
  "v=0\r\n" +
  "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n" +
  "b=AS:30000\r\n" +
  "a=rtpmap:96 AV1/90000\r\n";

const replacedSdp = injectVideoBandwidthSdp(sdpWithExistingBas, 80000);
assert.ok(replacedSdp.includes("b=AS:80000"), "Existing b=AS must be replaced with new bitrate");
assert.ok(!replacedSdp.includes("b=AS:30000"), "Old b=AS value must be replaced");

// Test 4: SDP with video line without c= line
const sdpNoCLine =
  "v=0\r\n" +
  "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n" +
  "a=rtpmap:96 AV1/90000\r\n";

const injectedNoC = injectVideoBandwidthSdp(sdpNoCLine, 50000);
assert.ok(
  injectedNoC.includes("m=video 9 UDP/TLS/RTP/SAVPF 96\r\nb=AS:50000\r\n"),
  "b=AS must be inserted directly after m=video line when c= is absent"
);

// Test 5: Headroom calculations (25%) across resolution/bitrate spectrum
const testBitrates = [500, 10000, 50000, 60000, 70000, 80000, 100000, 200000];
for (const br of testBitrates) {
  const cap = Math.round(br * 1.25);
  const result = injectVideoBandwidthSdp(sampleSdp, cap);
  assert.ok(result.includes("b=AS:" + cap + "\r\n"), "Headroom cap " + cap + " must be present in SDP for " + br + " kbps");
}

// Test 6: Fractional / decimal bitrates rounding
const fracSdp = injectVideoBandwidthSdp(sampleSdp, 87500.6);
assert.ok(fracSdp.includes("b=AS:87501\r\n"), "Floating-point bitrates must be rounded to integer");

// Test 7: SDP with Unix line endings (\n)
const unixSdp = "v=0\nm=video 9 UDP/TLS/RTP/SAVPF 96\nc=IN IP4 0.0.0.0\na=rtpmap:96 AV1/90000\n";
const unixInjected = injectVideoBandwidthSdp(unixSdp, 75000);
assert.ok(unixInjected.includes("b=AS:75000"), "b=AS must be correctly inserted into SDP with Unix line endings");

// Test 8: SDP with Audio only (no m=video)
const audioOnlySdp =
  "v=0\r\n" +
  "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n" +
  "c=IN IP4 0.0.0.0\r\n" +
  "a=rtpmap:111 opus/48000/2\r\n";
const audioResult = injectVideoBandwidthSdp(audioOnlySdp, 70000);
assert.strictEqual(audioResult, audioOnlySdp, "Audio-only SDP must remain untouched");

// Test 9: Idempotence (repeated calls must not duplicate b=AS lines)
const once = injectVideoBandwidthSdp(sampleSdp, 80000);
const twice = injectVideoBandwidthSdp(once, 80000);
assert.strictEqual(once, twice, "Subsequent calls with same bitrate must be idempotent");
const matches = twice.match(/b=AS:80000/g);
assert.strictEqual(matches.length, 1, "There must be exactly one b=AS line in the video section");

console.log("Tizen SDP bandwidth tests passed");

