import { describe, expect, it } from "vitest";
import { buildToolArgs } from "../../src/editor/editor-build.js";

/**
 * project(build) runs beside other editors. Without -NoHotReloadFromIDE any
 * running editor of the same engine with Live Coding on makes UnrealBuildTool
 * refuse the build, whichever project that editor has open.
 */
describe("the UnrealBuildTool arguments project(build) passes", () => {
  const base = { target: "GameEditor", platform: "Win64", configuration: "Development", projectPath: "C:/P/Game.uproject" };

  it("builds beside a running editor, on Windows and elsewhere", () => {
    for (const windows of [true, false]) {
      const args = buildToolArgs({ ...base, windows });
      expect(args).toContain("-NoHotReloadFromIDE");
      expect(args).toContain("-WaitMutex");
      expect(args.slice(0, 3)).toEqual(["GameEditor", "Win64", "Development"]);
    }
  });

  it("quotes the project only for the Windows command line", () => {
    expect(buildToolArgs({ ...base, windows: true })).toContain('-Project="C:/P/Game.uproject"');
    expect(buildToolArgs({ ...base, windows: false })).toContain("-Project=C:/P/Game.uproject");
  });

  it("passes -Clean only when asked", () => {
    expect(buildToolArgs({ ...base, windows: true })).not.toContain("-Clean");
    expect(buildToolArgs({ ...base, windows: true, clean: true })).toContain("-Clean");
  });
});
