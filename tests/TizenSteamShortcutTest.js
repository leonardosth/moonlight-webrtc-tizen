"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");

const rootDir = path.resolve(__dirname, "..");
const steamConfigPath = path.join(rootDir, "tizen-steam", "config.xml");
const steamAppPath = path.join(rootDir, "tizen-steam", "app.js");
const steamIconPath = path.join(rootDir, "tizen-steam", "assets", "steam-icon.png");
const moonlightConfigPath = path.join(rootDir, "tizen", "config.xml");
const moonlightAppPath = path.join(rootDir, "tizen", "app.js");

assert.ok(fs.existsSync(steamConfigPath), "tizen-steam/config.xml must exist");
assert.ok(fs.existsSync(steamAppPath), "tizen-steam/app.js must exist");
assert.ok(fs.existsSync(steamIconPath), "tizen-steam/assets/steam-icon.png must exist");

const steamConfig = fs.readFileSync(steamConfigPath, "utf8");
const steamApp = fs.readFileSync(steamAppPath, "utf8");
const moonlightConfig = fs.readFileSync(moonlightConfigPath, "utf8");
const moonlightApp = fs.readFileSync(moonlightAppPath, "utf8");

assert.ok(steamConfig.includes('package="MlWrtcStm1"'), "Steam shortcut package ID must be MlWrtcStm1");
assert.ok(steamConfig.includes('id="MlWrtcStm1.SteamBigPicture"'), "Steam shortcut application ID must be MlWrtcStm1.SteamBigPicture");
assert.ok(steamConfig.includes("<name>Steam Big Picture</name>"), "Steam shortcut visible name must be Steam Big Picture");
assert.ok(steamConfig.includes('name="http://tizen.org/privilege/application.launch"'), "Steam shortcut requires application.launch privilege");

assert.ok(steamApp.includes('TARGET_APP_ID = "MlWrtcTst1.MoonlightWebRTCTest"'), "Steam shortcut must launch Moonlight WebRTC application ID");
assert.ok(steamApp.includes('"autostart"') && steamApp.includes('["Steam"]'), "Steam shortcut must pass autostart Steam payload");

assert.ok(moonlightConfig.includes('<tizen:operation name="http://tizen.org/appcontrol/operation/view"/>'), "Moonlight config must declare app-control operation/view handler");

assert.ok(moonlightApp.includes("function checkRequestedAppControl()"), "Moonlight app must check requested app control on launch");
assert.ok(moonlightApp.includes("function triggerAutostart(targetAppName)"), "Moonlight app must implement triggerAutostart");
assert.ok(moonlightApp.includes("function attemptAutostartLaunch()"), "Moonlight app must implement attemptAutostartLaunch");
assert.ok(moonlightApp.includes("isAutostartSession"), "Moonlight app must track autostart sessions for auto-exit");
assert.ok(moonlightApp.includes("autostartEverStreamed"), "Moonlight app must track if stream has actually started before auto-exiting on idle");
assert.ok(moonlightApp.includes("showAutostartLaunching"), "Moonlight app must display the launching screen directly instead of flashing homeScreen");
assert.ok(moonlightApp.includes('includes("big picture")'), "Moonlight app autostart must support matching Steam Big Picture");

console.log("Tizen Steam shortcut tests passed");
