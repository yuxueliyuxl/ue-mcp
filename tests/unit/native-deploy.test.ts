/**
 * Reinstalling a native module removes what the previous version deployed and
 * the new one no longer ships (#1236), and leaves files the user added.
 */
import { afterEach, describe, expect, it } from "vitest";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { deployNativeModule, pruneStaleNativeFiles } from "../../src/extensions/native-deploy.js";

const temps: string[] = [];
function tempDir(): string {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "ue-mcp-native-"));
  temps.push(dir);
  return dir;
}
function write(file: string, content: string): void {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, content);
}

afterEach(() => {
  for (const dir of temps.splice(0)) fs.rmSync(dir, { recursive: true, force: true });
});

describe("native module redeploy", () => {
  it("removes files the new version no longer ships and keeps the rest", () => {
    const project = tempDir();
    const v1 = tempDir();
    const v2 = tempDir();
    write(path.join(v1, "ue/Source/Mod/A.h"), "a1");
    write(path.join(v1, "ue/Source/Mod/Old/B.h"), "b1");
    write(path.join(v2, "ue/Source/Mod/A.h"), "a2");

    const first = deployNativeModule(v1, "ue", "Plug", project);
    write(path.join(project, "Plugins/Plug/Source/Mod/UserAdded.h"), "mine");

    const second = deployNativeModule(v2, "ue", "Plug", project);
    const pruned = pruneStaleNativeFiles(project, first.fileList, second.fileList);

    expect(pruned).toBe(1);
    expect(fs.existsSync(path.join(project, "Plugins/Plug/Source/Mod/Old/B.h"))).toBe(false);
    expect(fs.existsSync(path.join(project, "Plugins/Plug/Source/Mod/Old"))).toBe(false);
    expect(fs.readFileSync(path.join(project, "Plugins/Plug/Source/Mod/A.h"), "utf-8")).toBe("a2");
    expect(fs.readFileSync(path.join(project, "Plugins/Plug/Source/Mod/UserAdded.h"), "utf-8")).toBe("mine");
  });

  it("removes nothing on a first install", () => {
    const project = tempDir();
    const v1 = tempDir();
    write(path.join(v1, "ue/A.h"), "a");
    const first = deployNativeModule(v1, "ue", "Plug", project);
    expect(pruneStaleNativeFiles(project, [], first.fileList)).toBe(0);
    expect(fs.existsSync(path.join(project, "Plugins/Plug/A.h"))).toBe(true);
  });
});
