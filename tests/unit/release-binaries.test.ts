import { describe, it, expect } from "vitest";
import { spawnSync } from "node:child_process";
import * as path from "node:path";
import { REQUIRED_ENGINES, missingBinaries, missingMessage, requiredBinaries } from "../../scripts/release-binaries.mjs";

const script = path.resolve(__dirname, "../../scripts/release-binaries.mjs");
const run = (version: string, assets: string[]) =>
  spawnSync(process.execPath, [script, "--version", version], { input: assets.join("\n"), encoding: "utf-8" });

describe("release binaries gate", () => {
  it("requires a Win64 bridge for each promised engine and the manifest", () => {
    expect(REQUIRED_ENGINES).toEqual(["5.7", "5.8"]);
    expect(requiredBinaries("1.4.0-beta.3")).toEqual([
      "UE_MCP_Bridge-1.4.0-beta.3-UE5.7-Win64.zip",
      "UE_MCP_Bridge-1.4.0-beta.3-UE5.8-Win64.zip",
      "UE_MCP_Bridge-1.4.0-beta.3-manifest.json",
    ]);
  });

  it("names what a release is missing, and nothing when it has all of it", () => {
    expect(missingBinaries("1.4.0-beta.2", ["ue-mcp.tar.gz"])).toEqual(requiredBinaries("1.4.0-beta.2"));
    expect(missingBinaries("1.4.0-beta.3", [...requiredBinaries("1.4.0-beta.3"), "ue-mcp.tar.gz"])).toEqual([]);
    // Another version's binaries do not count.
    expect(missingBinaries("1.4.0-beta.3", requiredBinaries("1.4.0-beta.1"))).toHaveLength(3);
  });

  it("says how to fix it", () => {
    const m = missingMessage("1.4.0-beta.2", ["UE_MCP_Bridge-1.4.0-beta.2-UE5.8-Win64.zip"]);
    expect(m).toContain("UE_MCP_Bridge-1.4.0-beta.2-UE5.8-Win64.zip");
    expect(m).toContain("npm run binaries:build");
    expect(m).toContain("npm run binaries:upload");
  });

  it("fails the publish step without the binaries and passes with them", () => {
    const bad = run("1.4.0-beta.2", ["ue-mcp.tar.gz"]);
    expect(bad.status).toBe(1);
    expect(bad.stderr).toContain("UE_MCP_Bridge-1.4.0-beta.2-manifest.json");
    const good = run("1.4.0-beta.3", [...requiredBinaries("1.4.0-beta.3"), ""]);
    expect(good.status).toBe(0);
  });
});
