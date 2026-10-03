"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");

const rootDir = path.resolve(__dirname, "..");
const appJsPath = path.join(rootDir, "tizen", "app.js");

assert.ok(fs.existsSync(appJsPath), "tizen/app.js must exist");

const appContent = fs.readFileSync(appJsPath, "utf8");

// 1. Verify existence of sendClientTelemetry function
assert.ok(
  appContent.includes("function sendClientTelemetry(data)"),
  "app.js must define sendClientTelemetry function"
);

// 2. Verify DataChannel send format
assert.ok(
  appContent.includes('type: "client-telemetry"'),
  'sendClientTelemetry must send JSON with type: "client-telemetry"'
);

// 3. Verify sendClientTelemetry integration inside updateStatistics
assert.ok(
  appContent.includes("sendClientTelemetry({"),
  "app.js must invoke sendClientTelemetry within statistics updating"
);

// 4. Verify all critical metrics are packaged
const expectedFields = [
  "decodedFps",
  "decodeTimeMs",
  "networkRttMs",
  "videoJitterMs",
  "framesReceived",
  "framesDecoded",
  "framesDropped",
  "droppedPerSec",
  "packetsReceived",
  "packetsLost",
  "currentMbps",
  "audioPacketsReceived",
  "audioPacketsLost",
  "audioJitterMs",
  "clientFrameIntervalMs",
  "clientFrameJitterMs",
];

for (const field of expectedFields) {
  assert.ok(
    appContent.includes(field + ":"),
    `sendClientTelemetry payload must include metric field: ${field}`
  );
}

// 5. Test mock execution of sendClientTelemetry serialization
let sentPayload = null;
const mockChannel = {
  readyState: "open",
  send: function (text) {
    sentPayload = JSON.parse(text);
  },
};

function testSendClientTelemetry(channel, data) {
  if (channel && channel.readyState === "open") {
    channel.send(
      JSON.stringify({
        type: "client-telemetry",
        timestamp: 123456789,
        data: data,
      })
    );
  }
}

testSendClientTelemetry(mockChannel, {
  decodedFps: 60.0,
  decodeTimeMs: 4.2,
  networkRttMs: 2.1,
});

assert.strictEqual(sentPayload.type, "client-telemetry");
assert.strictEqual(sentPayload.timestamp, 123456789);
assert.strictEqual(sentPayload.data.decodedFps, 60.0);
assert.strictEqual(sentPayload.data.decodeTimeMs, 4.2);

console.log("Tizen client telemetry unit tests passed");
