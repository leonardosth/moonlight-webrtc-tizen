(function (global) {
  "use strict";

  const STORAGE_KEY = "moonlight-webrtc.client.preferences.v1";
  const DEFAULTS = Object.freeze({
    resolution: null,
    fps: null,
    codec: null,
    hdr: false,
    bitrateKbps: null,
    latencyMode: "balanced",
    packetPacing: "balanced",
    frameInterpolation: false,
    streamingPreset: "balanced",
    sunshineEncryption: true,
    frameDropPolicy: "auto",
    videoJitterBufferTargetMs: 15,
    audioJitterBufferTargetMs: 40,
    rtpMaxFragmentSize: 1360,
    idrCooldownMs: 500,
    ulpfecPercentage: 0,
  });

  function copyDefaults() {
    return {
      resolution: DEFAULTS.resolution,
      fps: DEFAULTS.fps,
      codec: DEFAULTS.codec,
      hdr: DEFAULTS.hdr,
      bitrateKbps: DEFAULTS.bitrateKbps,
      latencyMode: DEFAULTS.latencyMode,
      packetPacing: DEFAULTS.packetPacing,
      frameInterpolation: DEFAULTS.frameInterpolation,
      streamingPreset: DEFAULTS.streamingPreset,
      sunshineEncryption: DEFAULTS.sunshineEncryption,
      frameDropPolicy: DEFAULTS.frameDropPolicy,
      videoJitterBufferTargetMs: DEFAULTS.videoJitterBufferTargetMs,
      audioJitterBufferTargetMs: DEFAULTS.audioJitterBufferTargetMs,
      rtpMaxFragmentSize: DEFAULTS.rtpMaxFragmentSize,
      idrCooldownMs: DEFAULTS.idrCooldownMs,
      ulpfecPercentage: DEFAULTS.ulpfecPercentage,
    };
  }

  function normalize(candidate) {
    const values = copyDefaults();
    if (!candidate || typeof candidate !== "object") {
      return values;
    }
    if (typeof candidate.resolution === "string") {
      values.resolution = candidate.resolution;
    }
    if (Number.isInteger(candidate.fps) && candidate.fps > 0) {
      values.fps = candidate.fps;
    }
    if (typeof candidate.codec === "string") {
      values.codec = candidate.codec;
    }
    if (typeof candidate.hdr === "boolean") {
      values.hdr = candidate.hdr;
    }
    if (Number.isInteger(candidate.bitrateKbps) && candidate.bitrateKbps > 0) {
      values.bitrateKbps = candidate.bitrateKbps;
    }
    if (typeof candidate.latencyMode === "string" && (candidate.latencyMode === "lowest_latency" || candidate.latencyMode === "balanced" || candidate.latencyMode === "smoothest")) {
      values.latencyMode = candidate.latencyMode;
    }
    if (typeof candidate.packetPacing === "string" && (candidate.packetPacing === "disabled" || candidate.packetPacing === "fast" || candidate.packetPacing === "balanced" || candidate.packetPacing === "aggressive")) {
      values.packetPacing = candidate.packetPacing;
    }
    if (typeof candidate.frameInterpolation === "boolean") {
      values.frameInterpolation = candidate.frameInterpolation;
    }
    if (typeof candidate.streamingPreset === "string" && (candidate.streamingPreset === "custom" || candidate.streamingPreset === "ultra_low_latency" || candidate.streamingPreset === "low_latency" || candidate.streamingPreset === "balanced" || candidate.streamingPreset === "smooth" || candidate.streamingPreset === "smoothest" || candidate.streamingPreset === "fast_wifi" || candidate.streamingPreset === "max_stability" || candidate.streamingPreset === "ethernet_100m")) {
      if (candidate.streamingPreset === "fast_wifi" || candidate.streamingPreset === "ethernet_100m") {
        values.streamingPreset = "low_latency";
      } else if (candidate.streamingPreset === "max_stability") {
        values.streamingPreset = "smoothest";
      } else {
        values.streamingPreset = candidate.streamingPreset;
      }
    }
    if (typeof candidate.sunshineEncryption === "boolean") {
      values.sunshineEncryption = candidate.sunshineEncryption;
    }
    if (typeof candidate.frameDropPolicy === "string" && (candidate.frameDropPolicy === "auto" || candidate.frameDropPolicy === "disabled" || candidate.frameDropPolicy === "tolerant" || candidate.frameDropPolicy === "strict")) {
      values.frameDropPolicy = candidate.frameDropPolicy;
    }
    if (Number.isInteger(candidate.videoJitterBufferTargetMs) && candidate.videoJitterBufferTargetMs >= 0) {
      values.videoJitterBufferTargetMs = candidate.videoJitterBufferTargetMs;
    }
    if (Number.isInteger(candidate.audioJitterBufferTargetMs) && candidate.audioJitterBufferTargetMs >= 0) {
      values.audioJitterBufferTargetMs = candidate.audioJitterBufferTargetMs;
    }
    if (Number.isInteger(candidate.rtpMaxFragmentSize) && candidate.rtpMaxFragmentSize >= 1000 && candidate.rtpMaxFragmentSize <= 1460) {
      values.rtpMaxFragmentSize = candidate.rtpMaxFragmentSize;
    }
    if (Number.isInteger(candidate.idrCooldownMs) && candidate.idrCooldownMs >= 100) {
      values.idrCooldownMs = candidate.idrCooldownMs;
    }
    if (Number.isInteger(candidate.ulpfecPercentage) && candidate.ulpfecPercentage >= 0 && candidate.ulpfecPercentage <= 50) {
      values.ulpfecPercentage = candidate.ulpfecPercentage;
    }
    return values;
  }

  function resolveSupported(value, allowedValues, fallbackValue) {
    return Array.isArray(allowedValues) && allowedValues.some(function (allowedValue) {
      return String(allowedValue) === String(value);
    }) ? value : fallbackValue;
  }

  function ClientPreferences(storage) {
    this.storage = storage || global.localStorage;
    this.values = copyDefaults();
  }

  ClientPreferences.prototype.load = function () {
    try {
      const serialized = this.storage && this.storage.getItem(STORAGE_KEY);
      this.values = serialized ? normalize(JSON.parse(serialized)) : copyDefaults();
    } catch (error) {
      this.values = copyDefaults();
    }
    return this.snapshot();
  };

  ClientPreferences.prototype.snapshot = function () {
    return normalize(this.values);
  };

  ClientPreferences.prototype.replace = function (values) {
    this.values = normalize(values);
    try {
      if (this.storage) {
        this.storage.setItem(STORAGE_KEY, JSON.stringify(this.values));
      }
    } catch (error) {
      // Storage can be disabled by the TV runtime. In-memory preferences still work.
    }
    return this.snapshot();
  };

  ClientPreferences.prototype.update = function (key, value) {
    const values = this.snapshot();
    values[key] = value;
    return this.replace(values);
  };

  ClientPreferences.prototype.clear = function () {
    this.values = copyDefaults();
    try {
      if (this.storage) {
        this.storage.removeItem(STORAGE_KEY);
      }
    } catch (error) {
      // Keep the in-memory defaults when persistent storage is unavailable.
    }
    return this.snapshot();
  };

  global.ClientPreferences = {
    STORAGE_KEY: STORAGE_KEY,
    create: function (storage) { return new ClientPreferences(storage); },
    resolveSupported: resolveSupported,
    testing: {
      defaults: copyDefaults,
      normalize: normalize,
      resolveSupported: resolveSupported,
    },
  };
}(window));
