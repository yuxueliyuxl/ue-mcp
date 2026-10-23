import { describe, it, expect } from "vitest";
import * as fs from "node:fs";
import * as path from "node:path";
import { packageModulePath, packageRoot, packageVersion } from "../../src/core/package-root.js";

describe("packageRoot", () => {
  it("is the directory shipping package.json, the bridge plugin and the skills", () => {
    const root = packageRoot();
    expect(fs.existsSync(path.join(root, "package.json"))).toBe(true);
    expect(fs.existsSync(path.join(root, "plugin", "ue_mcp_bridge", "UE_MCP_Bridge.uplugin"))).toBe(true);
    expect(fs.statSync(path.join(root, "skills")).isDirectory()).toBe(true);
  });

  it("resolves top-level entry points in src/, whatever folder this module is in", () => {
    const entry = packageModulePath("index.js");
    expect(path.dirname(entry)).toBe(path.join(packageRoot(), "src"));
    expect(fs.existsSync(entry.replace(/\.js$/, ".ts"))).toBe(true);
  });
});

describe("packageVersion", () => {
  it("reads the version from the package's own package.json", () => {
    const pkg = JSON.parse(fs.readFileSync(path.join(packageRoot(), "package.json"), "utf-8")) as { version: string };
    expect(packageVersion()).toBe(pkg.version);
  });
});
