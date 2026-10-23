import { describe, expect, it } from "vitest";
import { editorTool } from "../../src/tools/editor.js";
import { readHandlerFile } from "../../scripts/lib/cpp-registrations.mjs";

describe("run_python_file as __main__ and viewport camera locks (#1361)", () => {
  it("declares runName and asMain on run_python_file", () => {
    expect(editorTool.schema.runName.safeParse("__main__").success).toBe(true);
    expect(editorTool.schema.asMain.safeParse(true).success).toBe(true);
    expect(editorTool.actions.run_python_file.description).toContain("asMain=true");
  });

  it("runs the file through runpy under the requested run name", () => {
    const source = readHandlerFile("EditorHandlers.cpp");
    expect(source).toContain('RunName = TEXT("__main__");');
    expect(source).toContain("_uemcp_runpy.run_path(_uemcp_path, run_name=%s)");
    expect(source).toContain("_uemcp_sys.argv = [_uemcp_path] + _uemcp_json.loads(%s)");
  });

  it("reports a piloted or locked viewport from set_viewport and capture_screenshot", () => {
    const source = readHandlerFile("EditorHandlers.cpp");
    expect(source.match(/DescribeViewportCameraLock\(ViewportClient\)/g)?.length).toBeGreaterThanOrEqual(2);
    expect(source).toContain("GetActiveActorLock()");
    expect(editorTool.actions.set_viewport.description).toContain("cameraLock");
    expect(editorTool.actions.capture_screenshot.description).toContain("cameraLock");
  });
});
