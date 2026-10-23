/**
 * Guards declared in ue-mcp.yml, on a real server with a bridge behind it.
 * The guard is a shell command that exits non-zero, so a refusal comes from
 * the configured hook and nothing else. What is asserted is what the bridge
 * received: a refused call must never reach the editor.
 */
import * as fs from "node:fs";
import * as path from "node:path";
import { afterAll, beforeAll, describe, expect, it } from "vitest";
import { covers } from "../helpers/covers.js";
import { FakeBridge } from "../fake-bridge.js";
import { json, McpServer, Sandbox } from "./harness.js";

const guarded = (scope: string) => `ue-mcp:
  version: 1
guards:
  policy:
    scope: ${scope}
    before:
      class_path: shell
      options:
        command: exit 3
flows:
  spawn_light:
    description: One mutating step.
    steps:
      "1":
        task: level.place_actor
        options:
          actorClass: PointLight
`;

let sb: Sandbox;
let mutationsBridge: FakeBridge;
let writesBridge: FakeBridge;
let mutations: McpServer;
let writes: McpServer;
let writesProjectDir: string;

const made = (b: FakeBridge) => b.methods.filter((m) => m !== "list_dialogs");
const call = (s: McpServer, category: string, method: string, args: Record<string, unknown> = {}) =>
  s.call("tools", { action: "call", category, method, args });

beforeAll(async () => {
  sb = new Sandbox();
  const mutationsProject = sb.makeProject("Frozen", { config: guarded("mutations") });
  const writesProject = sb.makeProject("Tracked", { config: guarded("writes"), content: true });
  writesProjectDir = path.dirname(writesProject);
  fs.writeFileSync(path.join(writesProjectDir, "Content", "Existing.uasset"), "not a real package");
  [mutationsBridge, writesBridge] = await Promise.all([
    FakeBridge.start({ projectDir: path.dirname(mutationsProject) }),
    FakeBridge.start({ projectDir: writesProjectDir }),
  ]);
  [mutations, writes] = await Promise.all([
    McpServer.start(sb, { projects: [mutationsProject], port: null }),
    McpServer.start(sb, { projects: [writesProject], port: null }),
  ]);
}, 60_000);

afterAll(async () => {
  await mutations?.close();
  await writes?.close();
  await Promise.all([mutationsBridge?.stop(), writesBridge?.stop()]);
  sb.cleanup();
});

describe("a guard from ue-mcp.yml", () => {
  it("refuses the calls it covers before they reach the editor and lets the rest through", async () => {
    covers("guards.config");
    mutationsBridge.calls.length = 0;

    const spawn = await call(mutations, "level", "place_actor", { actorClass: "PointLight" });
    expect(spawn.isError).toBe(true);
    expect(spawn.text).toContain("blocked (place_actor)");
    const del = await call(mutations, "asset", "delete", { assetPath: "/Game/Anything" });
    expect(del.isError).toBe(true);
    expect(del.text).toContain("blocked (delete_asset)");
    expect(made(mutationsBridge)).toEqual([]);

    const read = await call(mutations, "asset", "list_textures", { directory: "/Game" });
    expect(read.isError).toBe(false);
    expect(json(read).method).toBe("list_textures");
    expect(made(mutationsBridge)).toEqual(["list_textures"]);
  });

  it("guards a flow's steps the same way", async () => {
    covers("guards.config");
    mutationsBridge.calls.length = 0;
    const run = json<{ success: boolean; steps: Array<{ success: boolean; error?: string }> }>(
      await mutations.call("flow", { action: "run", flowName: "spawn_light" }),
    );
    expect(run.success).toBe(false);
    expect(JSON.stringify(run.steps[0])).toContain("blocked (place_actor)");
    expect(made(mutationsBridge)).toEqual([]);
  });
});

describe("write detection", () => {
  it("scopes a writes guard to calls that modify existing content, and names the file", async () => {
    covers("guards.write-detect");
    writesBridge.calls.length = 0;

    const existing = await call(writes, "asset", "delete", { assetPath: "/Game/Existing" });
    expect(existing.isError).toBe(true);
    expect(existing.text).toContain("blocked (delete_asset)");
    expect(existing.text).toContain(path.join(writesProjectDir, "Content", "Existing.uasset"));
    expect(made(writesBridge)).toEqual([]);

    // Nothing on disk to modify, and a mutation that names no content: both pass a writes guard.
    const absent = await call(writes, "asset", "delete", { assetPath: "/Game/NotOnDisk" });
    expect(absent.isError).toBe(false);
    const spawn = await call(writes, "level", "place_actor", { actorClass: "PointLight" });
    expect(spawn.isError).toBe(false);
    const read = await call(writes, "asset", "list_textures", { directory: "/Game" });
    expect(read.isError).toBe(false);
    expect(made(writesBridge)).toEqual(["delete_asset", "place_actor", "list_textures"]);
  });
});
