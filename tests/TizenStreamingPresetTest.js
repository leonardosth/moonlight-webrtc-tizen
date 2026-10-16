"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");

const rootDir = path.resolve(__dirname, "..");
const appJsPath = path.join(rootDir, "tizen", "app.js");
const indexHtmlPath = path.join(rootDir, "tizen", "index.html");
const preferencesJsPath = path.join(rootDir, "tizen", "preferences.js");

assert.ok(fs.existsSync(appJsPath), "tizen/app.js must exist");
assert.ok(fs.existsSync(indexHtmlPath), "tizen/index.html must exist");
assert.ok(fs.existsSync(preferencesJsPath), "tizen/preferences.js must exist");

const appContent = fs.readFileSync(appJsPath, "utf8");
const indexHtmlContent = fs.readFileSync(indexHtmlPath, "utf8");

// 1. Verify existence of preset constants and functions in app.js
assert.ok(
  appContent.includes("const STREAMING_PRESET_DESCRIPTIONS = {"),
  "app.js must define STREAMING_PRESET_DESCRIPTIONS"
);
assert.ok(
  appContent.includes("function applyStreamingPreset(preset)"),
  "app.js must define applyStreamingPreset"
);

// 2. Extract STREAMING_PRESET_DESCRIPTIONS
const descriptionsMatch = appContent.match(/const STREAMING_PRESET_DESCRIPTIONS = \{[\s\S]*?\n\};/);
assert.ok(descriptionsMatch, "STREAMING_PRESET_DESCRIPTIONS object body must be matched");
const STREAMING_PRESET_DESCRIPTIONS = new Function(
  descriptionsMatch[0] + "\nreturn STREAMING_PRESET_DESCRIPTIONS;"
)();

// 3. Verify all expected presets have informative descriptions
const expectedPresets = ["custom", "ultra_low_latency", "low_latency", "balanced", "smooth", "smoothest"];
for (const preset of expectedPresets) {
  assert.ok(
    typeof STREAMING_PRESET_DESCRIPTIONS[preset] === "string" && STREAMING_PRESET_DESCRIPTIONS[preset].length > 10,
    "Preset " + preset + " must have a non-empty descriptive string"
  );
  assert.ok(
    indexHtmlContent.includes('value="' + preset + '"'),
    "index.html must have an <option> for preset: " + preset
  );
}

// 4. Test simulate applyStreamingPreset function logic with mock DOM elements
function createMockSelect(initialValue) {
  return {
    value: initialValue,
    disabled: false,
  };
}

function createMockElement(initialText) {
  return {
    textContent: initialText || "",
  };
}

// Extract applyStreamingPreset body
const fnMatch = appContent.match(/function applyStreamingPreset\(preset\) \{[\s\S]*?\n\}/);
assert.ok(fnMatch, "applyStreamingPreset body must be found");

// Setup isolated sandbox environment
function runPresetTest(presetToApply, partialDom) {
  const sandbox = {
    streamingPresetSelect: createMockSelect("balanced"),
    latencyModeSelect: partialDom && partialDom.latencyModeSelect === null ? null : createMockSelect("balanced"),
    packetPacingSelect: partialDom && partialDom.packetPacingSelect === null ? null : createMockSelect("balanced"),
    encryptionSelect: partialDom && partialDom.encryptionSelect === null ? null : createMockSelect("true"),
    frameDropPolicySelect: partialDom && partialDom.frameDropPolicySelect === null ? null : createMockSelect("auto"),
    videoJitterBufferSelect: partialDom && partialDom.videoJitterBufferSelect === null ? null : createMockSelect("15"),
    audioJitterBufferSelect: partialDom && partialDom.audioJitterBufferSelect === null ? null : createMockSelect("40"),
    rtpFragmentSizeSelect: partialDom && partialDom.rtpFragmentSizeSelect === null ? null : createMockSelect("1360"),
    idrCooldownSelect: partialDom && partialDom.idrCooldownSelect === null ? null : createMockSelect("500"),
    streamingPresetStatusElement: createMockElement(),
    latencyModeStatusElement: createMockElement(),
    packetPacingStatusElement: createMockElement(),
    encryptionStatusElement: createMockElement(),
    frameDropPolicyStatusElement: createMockElement(),
    videoJitterBufferStatusElement: createMockElement(),
    audioJitterBufferStatusElement: createMockElement(),
    rtpFragmentSizeStatusElement: createMockElement(),
    idrCooldownStatusElement: createMockElement(),
    updateLatencyModeStatus: function () {},
    updatePacketPacingStatus: function () {},
    updateEncryptionStatus: function () {},
    updateFrameDropPolicyStatus: function () {},
    updateVideoJitterBufferStatus: function () {},
    updateAudioJitterBufferStatus: function () {},
    updateRtpFragmentSizeStatus: function () {},
    updateIdrCooldownStatus: function () {},
    applyLowLatencyReceivers: function () {},
  };

  const runner = new Function(
    "sandbox",
    "with(sandbox) { " + fnMatch[0] + "; applyStreamingPreset('" + presetToApply + "'); }"
  );
  runner(sandbox);
  return sandbox;
}

// Test 1 (Nominal): Ultra-Low Latency preset configuration
const ullResult = runPresetTest("ultra_low_latency");
assert.strictEqual(ullResult.latencyModeSelect.value, "lowest_latency", "Ultra-low latency must use lowest_latency mode");
assert.strictEqual(ullResult.packetPacingSelect.value, "disabled", "Ultra-low latency must disable packet pacer");
assert.strictEqual(ullResult.encryptionSelect.value, "false", "Ultra-low latency must disable loopback encryption");
assert.strictEqual(ullResult.frameDropPolicySelect.value, "disabled", "Ultra-low latency must disable frame dropping");
assert.strictEqual(ullResult.videoJitterBufferSelect.value, "8", "Ultra-low latency must use 8ms video jitter buffer to absorb RTCP NACK");
assert.strictEqual(ullResult.audioJitterBufferSelect.value, "40", "Ultra-low latency must use 40ms audio jitter buffer");
assert.strictEqual(ullResult.rtpFragmentSizeSelect.value, "1200", "Ultra-low latency must use 1200B fragments for MTU safety");
assert.strictEqual(ullResult.idrCooldownSelect.value, "750", "Ultra-low latency must use 750ms IDR cooldown");

// Test 2 (Nominal): Low Latency preset configuration
const lowResult = runPresetTest("low_latency");
assert.strictEqual(lowResult.latencyModeSelect.value, "lowest_latency", "Low Latency must use lowest_latency mode");
assert.strictEqual(lowResult.packetPacingSelect.value, "fast", "Low Latency must use fast pacing (4x delivery window)");
assert.strictEqual(lowResult.encryptionSelect.value, "false", "Low Latency disables loopback encryption on LAN");
assert.strictEqual(lowResult.frameDropPolicySelect.value, "tolerant", "Low Latency uses tolerant drop policy");
assert.strictEqual(lowResult.videoJitterBufferSelect.value, "8", "Low Latency uses 8ms video jitter buffer to absorb NACK retransmission");
assert.strictEqual(lowResult.audioJitterBufferSelect.value, "40", "Low Latency uses 40ms audio jitter buffer");
assert.strictEqual(lowResult.rtpFragmentSizeSelect.value, "1360", "Low Latency uses 1360B MTU");
assert.strictEqual(lowResult.idrCooldownSelect.value, "500", "Low Latency uses 500ms IDR cooldown");

// Test 3 (Nominal): Balanced preset configuration
const balResult = runPresetTest("balanced");
assert.strictEqual(balResult.latencyModeSelect.value, "balanced");
assert.strictEqual(balResult.packetPacingSelect.value, "balanced");
assert.strictEqual(balResult.encryptionSelect.value, "true");
assert.strictEqual(balResult.frameDropPolicySelect.value, "auto");
assert.strictEqual(balResult.videoJitterBufferSelect.value, "15");
assert.strictEqual(balResult.audioJitterBufferSelect.value, "40");
assert.strictEqual(balResult.rtpFragmentSizeSelect.value, "1360");
assert.strictEqual(balResult.idrCooldownSelect.value, "500");

// Test 4 (Nominal): Smooth preset configuration
const smoothResult = runPresetTest("smooth");
assert.strictEqual(smoothResult.latencyModeSelect.value, "smooth");
assert.strictEqual(smoothResult.packetPacingSelect.value, "balanced");
assert.strictEqual(smoothResult.encryptionSelect.value, "true");
assert.strictEqual(smoothResult.frameDropPolicySelect.value, "tolerant");
assert.strictEqual(smoothResult.videoJitterBufferSelect.value, "30");
assert.strictEqual(smoothResult.audioJitterBufferSelect.value, "40");
assert.strictEqual(smoothResult.rtpFragmentSizeSelect.value, "1360");
assert.strictEqual(smoothResult.idrCooldownSelect.value, "500");

// Test 5 (Nominal): Smoothest preset configuration
const smoothestResult = runPresetTest("smoothest");
assert.strictEqual(smoothestResult.latencyModeSelect.value, "smooth");
assert.strictEqual(smoothestResult.packetPacingSelect.value, "aggressive");
assert.strictEqual(smoothestResult.encryptionSelect.value, "true");
assert.strictEqual(smoothestResult.frameDropPolicySelect.value, "tolerant");
assert.strictEqual(smoothestResult.videoJitterBufferSelect.value, "30");
assert.strictEqual(smoothestResult.audioJitterBufferSelect.value, "80");
assert.strictEqual(smoothestResult.rtpFragmentSizeSelect.value, "1200");
assert.strictEqual(smoothestResult.idrCooldownSelect.value, "750");

// Test 5 (Edge Case): 'custom' preset does not overwrite existing options
const customResult = runPresetTest("custom");
assert.strictEqual(customResult.packetPacingSelect.value, "balanced", "custom preset must not overwrite controls");
assert.strictEqual(customResult.videoJitterBufferSelect.value, "15", "custom preset must preserve existing values");

// Test 6 (Edge Case): Invalid or non-existent preset name should exit gracefully
const invalidResult = runPresetTest("unknown_scenario_xyz");
assert.strictEqual(invalidResult.packetPacingSelect.value, "balanced", "invalid preset must do nothing");

// Test 7 (Edge Case): Robustness against missing DOM elements (null controls)
const missingDomResult = runPresetTest("ultra_low_latency", {
  latencyModeSelect: null,
  packetPacingSelect: null,
  encryptionSelect: null,
});
assert.strictEqual(missingDomResult.videoJitterBufferSelect.value, "8", "Missing elements must not prevent updating remaining controls");

console.log("Tizen streaming preset tests passed");
