/**
 * The publish gate for prebuilt bridge binaries: a release must carry a bridge
 * zip for every engine it promises and the SHA-256 manifest the installer
 * checks, or every binary install's update fails with "no prebuilt bridge".
 *
 *   gh release view vX.Y.Z --json assets --jq '.assets[].name' \
 *     | node scripts/release-binaries.mjs --version X.Y.Z
 *
 * Reads the draft's asset names, one per line, on stdin. Exits 1 naming each
 * missing file and how to build and attach them.
 */
import * as fs from "node:fs";
import { pathToFileURL } from "node:url";

/** The engines every release ships a Win64 bridge for. */
export const REQUIRED_ENGINES = ["5.7", "5.8"];
export const PLATFORM = "Win64";

/** The files a release for `version` must carry. */
export function requiredBinaries(version, engines = REQUIRED_ENGINES, platform = PLATFORM) {
  return [
    ...engines.map((e) => `UE_MCP_Bridge-${version}-UE${e}-${platform}.zip`),
    `UE_MCP_Bridge-${version}-manifest.json`,
  ];
}

/** The required files not among `assets`. */
export function missingBinaries(version, assets, engines = REQUIRED_ENGINES, platform = PLATFORM) {
  const have = new Set(assets.map((a) => a.trim()).filter(Boolean));
  return requiredBinaries(version, engines, platform).filter((f) => !have.has(f));
}

/** The failure text for a release missing `missing`. */
export function missingMessage(version, missing) {
  return [
    `Release v${version} does not carry its prebuilt bridge binaries. Missing: ${missing.join(", ")}.`,
    "Build and attach them to the draft before bumping the version:",
    "  npm run binaries:build",
    "  npm run binaries:upload",
  ].join("\n");
}

function main() {
  const i = process.argv.indexOf("--version");
  const version = i >= 0 ? process.argv[i + 1] : null;
  if (!version) {
    console.error("::error::usage: node scripts/release-binaries.mjs --version X.Y.Z < asset-names");
    process.exit(1);
  }
  const assets = fs.readFileSync(0, "utf8").split(/\r?\n/);
  const missing = missingBinaries(version, assets);
  if (missing.length) {
    console.error(`::error::${missingMessage(version, missing).split("\n").join("%0A")}`);
    console.error(missingMessage(version, missing));
    process.exit(1);
  }
  console.log(`Release v${version} carries ${requiredBinaries(version).join(", ")}.`);
}

const invokedAsScript = process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href;
if (invokedAsScript) main();
