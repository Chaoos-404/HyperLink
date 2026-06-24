#!/usr/bin/env node

const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");

function platformName() {
  if (process.platform === "darwin") return "macos";
  if (process.platform === "linux") return "linux";
  if (process.platform === "win32") return "windows";
  return process.platform;
}

function archName() {
  if (process.arch === "x64") return "x64";
  if (process.arch === "arm64") return "arm64";
  return process.arch;
}

function executableName() {
  return process.platform === "win32" ? "hyperlink.exe" : "hyperlink";
}

function nativeDirCandidates() {
  const target = `${platformName()}-${archName()}`;
  return [
    path.join(__dirname, "prebuilds", target),
    path.join(__dirname, "..", "build", "release"),
    path.join(__dirname, "..", "build", "debug")
  ];
}

function chmodNativeTools(dir) {
  if (process.platform === "win32") return;
  for (const entry of fs.readdirSync(dir)) {
    if (entry === "hyperlink" || entry.startsWith("hyperlink_")) {
      try {
        fs.chmodSync(path.join(dir, entry), 0o755);
      } catch {
        // If chmod is blocked by the filesystem, try to run the binary anyway.
      }
    }
  }
}

function findNativeExecutable() {
  const executable = executableName();
  for (const dir of nativeDirCandidates()) {
    const candidate = path.join(dir, executable);
    if (fs.existsSync(candidate)) {
      chmodNativeTools(dir);
      return candidate;
    }
  }

  const target = `${platformName()}-${archName()}`;
  throw new Error(
    `No Hyperlink native binary is available for ${target}. ` +
      "This package currently ships linux-x64, macos-x64, macos-arm64, and windows-x64 builds."
  );
}

const native = findNativeExecutable();
const result = spawnSync(native, process.argv.slice(2), {
  stdio: "inherit",
  windowsHide: false
});

if (result.error) {
  console.error(result.error.message);
  process.exit(1);
}

process.exit(result.status ?? 1);
