/**
 * The retired Python bridge's Content/Python is removed from a deployed plugin
 * (#1273). Its init_unreal.py ran in commandlets and logged an Error, which
 * made a passing commandlet exit 1. deploy mirrors a source install already;
 * a binary install and an attach to an existing install did not clean it.
 */
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import * as fs from "node:fs";
import * as path from "node:path";
import { attach } from "../../src/editor/deployer.js";
import { legacyPythonDir, removeLegacyPythonContent } from "../../src/editor/legacy-python.js";
import { ProjectContext } from "../../src/config/project.js";
import { ProjectFixture } from "../helpers/project-fixture.js";

let fixture: ProjectFixture;

beforeEach(() => {
  fixture = new ProjectFixture("ue-mcp-legacy-python-");
});

afterEach(() => {
  fixture.cleanup();
});

function installBridgeWithPython(uproject: string): string {
  const dir = path.join(path.dirname(uproject), "Plugins", "UE_MCP_Bridge");
  fs.mkdirSync(legacyPythonDir(dir), { recursive: true });
  fs.writeFileSync(path.join(legacyPythonDir(dir), "init_unreal.py"), "import websockets\n");
  fs.writeFileSync(
    path.join(dir, "UE_MCP_Bridge.uplugin"),
    JSON.stringify({ FileVersion: 3, VersionName: "1.0.0", FriendlyName: "UE MCP Bridge" }, null, 2),
  );
  return dir;
}

describe("removeLegacyPythonContent", () => {
  it("removes Content/Python and the Content directory it leaves empty", () => {
    const dir = installBridgeWithPython(fixture.makeProject("Stale"));
    expect(removeLegacyPythonContent(dir)).toBe(true);
    expect(fs.existsSync(legacyPythonDir(dir))).toBe(false);
    expect(fs.existsSync(path.join(dir, "Content"))).toBe(false);
    expect(fs.existsSync(path.join(dir, "UE_MCP_Bridge.uplugin"))).toBe(true);
  });

  it("keeps other Content and reports nothing to do on a clean plugin", () => {
    const dir = installBridgeWithPython(fixture.makeProject("Mixed"));
    fs.writeFileSync(path.join(dir, "Content", "Keep.uasset"), "x");
    expect(removeLegacyPythonContent(dir)).toBe(true);
    expect(fs.existsSync(path.join(dir, "Content", "Keep.uasset"))).toBe(true);
    expect(removeLegacyPythonContent(dir)).toBe(false);
  });

  it("is applied when the server attaches to an existing install", () => {
    const uproject = fixture.makeProject("Attach");
    const dir = installBridgeWithPython(uproject);
    const context = new ProjectContext();
    context.setProject(uproject);
    attach(context);
    expect(fs.existsSync(legacyPythonDir(dir))).toBe(false);
  });
});

describe("the bridge module in a commandlet", () => {
  it("returns from StartupModule before starting the server, the hooks or the dialog hook", () => {
    const source = fs.readFileSync(
      path.join(__dirname, "..", "..", "plugin", "ue_mcp_bridge", "Source", "UE_MCP_Bridge", "Private", "UE_MCP_Bridge.cpp"),
      "utf8",
    );
    const startup = source.slice(source.indexOf("void FUE_MCP_BridgeModule::StartupModule()"));
    const guard = startup.indexOf("if (IsRunningCommandlet())");
    expect(guard).toBeGreaterThan(-1);
    for (const call of ["MakeShared<FMCPBridgeServer>", "FMCPEngineStatusHooks::Install()", "FDialogHandlers::InstallDialogHook()"]) {
      expect(startup.indexOf(call), call).toBeGreaterThan(guard);
    }
  });
});
