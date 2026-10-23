/**
 * #1384: a launch stopped at the engine's pre-bridge rebuild prompt says so,
 * and an editor's log says when it is exiting.
 */
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { MISSING_MODULE_PROMPT, readLogState } from "../../src/editor/engine-observer.js";
import { blockedStartupReason } from "../../src/editor/editor-control.js";

const roots: string[] = [];
afterEach(() => {
  while (roots.length > 0) fs.rmSync(roots.pop()!, { recursive: true, force: true });
});

/** A project whose log holds these lines, last written `ageSeconds` ago. */
function projectWithLog(lines: string[], ageSeconds = 0): string {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "ue-mcp-1384-"));
  roots.push(root);
  const uproject = path.join(root, "Vale.uproject");
  fs.writeFileSync(uproject, "{}");
  const logs = path.join(root, "Saved", "Logs");
  fs.mkdirSync(logs, { recursive: true });
  const log = path.join(logs, "Vale.log");
  fs.writeFileSync(log, lines.join("\r\n"));
  const at = (Date.now() - ageSeconds * 1000) / 1000;
  fs.utimesSync(log, at, at);
  return uproject;
}

describe("a launch stopped at the engine's rebuild prompt (Unreal 5.8)", () => {
  const tail = [
    "LogPluginManager: Mounting Project plugin ValeJourney",
    "LogInit: Running DelayedAutoRegister Phase EarliestPossiblePluginsLoaded",
    "LogInit: Warning: Incompatible or missing module: ValeJourney",
  ];

  it("is blocking once the log has gone quiet after the warning, and names the module", () => {
    const state = readLogState(projectWithLog(tail, 10));
    expect(state.phase).toBe(MISSING_MODULE_PROMPT);
    expect(state.blocking).toBe(true);
    expect(state.missingModules).toEqual(["ValeJourney"]);
    const reason = blockedStartupReason(state, { processes: [{ pid: 59828 } as never] });
    expect(reason).toMatch(/editor \(PID 59828\) is stopped at the engine's "modules are missing/);
    expect(reason).toMatch(/Not built: ValeJourney\. Build the project with project\(build\)/);
  });

  it("is not blocking while the log is still being written, or once startup goes on past it", () => {
    expect(readLogState(projectWithLog(tail, 0)).blocking).toBe(false);
    const onward = readLogState(projectWithLog([...tail, "LogModuleManager: Shutting down and abandoning module X"], 30));
    expect(onward.blocking).toBe(false);
  });
});

describe("an editor exiting, by its log", () => {
  it("is read off the exit lines", () => {
    expect(readLogState(projectWithLog(["LogSlate: Window 'Save Content' being destroyed", "Cmd: QUIT_EDITOR", "LogCore: Engine exit requested (reason: UUnrealEdEngine::CloseEditor())"])).exiting).toBe(true);
    expect(readLogState(projectWithLog(["LogExit: Exiting.", "Log file closed, 10/14/26 17:27:57"])).exiting).toBe(true);
    expect(readLogState(projectWithLog(["LogSlate: Window 'Save Content' being destroyed"])).exiting).toBe(false);
  });
});

