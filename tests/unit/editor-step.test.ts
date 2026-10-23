/** #1384: a step a handler failed by returning success: false fails the daemon's editor operation. */
import { describe, expect, it } from "vitest";
import { editorStepFailure, stepPayload } from "../../src/daemon/editor-step.js";

describe("a step of a daemon editor operation", () => {
  const json = (v: unknown) => JSON.stringify(v, null, 2);

  it("failed when the dispatch says so, or when its handler returned success: false", () => {
    expect(editorStepFailure(true, "Error [TASK_FAILED]: boom")).toBe("Error [TASK_FAILED]: boom");
    expect(editorStepFailure(false, json({ success: false, exitCode: 6, message: "Build failed with exit code 6" }))).toBe("Build failed with exit code 6");
    expect(editorStepFailure(false, `${json({ success: false, message: "Editor launched but did not become ready: x" })}\nserved by editor Vale`)).toBe("Editor launched but did not become ready: x");
    expect(editorStepFailure(false, json({ success: true }))).toBeNull();
  });

  it("went through when there was nothing to do", () => {
    expect(editorStepFailure(false, json({ success: false, alreadyStopped: true, message: "Editor is not running" }))).toBeNull();
    expect(editorStepFailure(false, json({ success: false, alreadyRunning: true, bridgeReady: true }))).toBeNull();
    expect(editorStepFailure(false, json({ success: false, alreadyRunning: true, bridgeReady: false, message: "up, no bridge" }))).toBe("up, no bridge");
  });

  it("reads the handler's JSON ahead of the notes the dispatch appends", () => {
    expect(stepPayload(`${json({ a: { b: 1 } })}\nnote`)).toEqual({ a: { b: 1 } });
    expect(stepPayload("not json")).toBeNull();
  });
});
