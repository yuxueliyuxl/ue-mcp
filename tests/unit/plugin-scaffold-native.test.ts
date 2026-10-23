/** The scaffold's dormant native module activates with a typed contract (#1282). */
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { describe, expect, it } from "vitest";
import yaml from "js-yaml";
import { writeScaffold } from "../../src/extensions/plugin-scaffold.js";
import { parseManifest } from "../../src/extensions/manifest.js";
import { nativeHandlerSurface } from "../../src/extensions/loader.js";
import { recordedSpecsProblems, type RecordedHandlerSpecs } from "../../src/surface/handler-spec.js";

describe("the scaffolded native module", () => {
  it("ships a recording its uncommented manifest block loads with", () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "ue-mcp-scaffold-"));
    try {
      writeScaffold(dir, "ue-mcp-probe", "probe");
      const recorded = JSON.parse(fs.readFileSync(path.join(dir, "handler-specs.json"), "utf8")) as RecordedHandlerSpecs;
      expect(recordedSpecsProblems(recorded)).toEqual([]);

      // Uncomment the dormant block, as the README tells the author to.
      const text = fs.readFileSync(path.join(dir, "ue-mcp.plugin.yml"), "utf8");
      const start = text.indexOf("# nativeModule:");
      const activated = text.slice(0, start)
        + text.slice(start).split("\n").map((l) => l.replace(/^# ?/, "")).join("\n");
      const { manifest, dropped } = parseManifest(yaml.load(activated));
      expect(dropped).toEqual([]);
      expect(manifest.nativeModule?.specs).toBe("handler-specs.json");
      expect(manifest.nativeModule?.minBridgeApi).toBe(2);

      const surface = nativeHandlerSurface(manifest, "ue-mcp-probe", new Set(), recorded.handlers);
      if (surface?.kind !== "provide") throw new Error("expected a provided category");
      expect(surface.plan.spec.actions.echo.contract?.spec).toEqual(recorded.handlers.echo);

      const cpp = fs.readdirSync(path.join(dir, "ue", "Plugins"), { recursive: true, encoding: "utf8" })
        .filter((f) => f.endsWith("Module.cpp"))
        .map((f) => fs.readFileSync(path.join(dir, "ue", "Plugins", f), "utf8"))
        .join("\n");
      expect(cpp).toContain(`MCPParam::Required(TEXT("name"), EMCPParamType::String, TEXT("Name to echo"))`);
    } finally {
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });
});
