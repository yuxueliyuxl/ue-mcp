import { describe, expect, it } from "vitest";
import { materialTool } from "../../src/tools/material.js";
import { readHandlerFile } from "../../scripts/lib/cpp-registrations.mjs";

describe("material.read_instance assetPaths batch (#1183)", () => {
  it("declares assetPaths and overridesOnly", () => {
    expect(materialTool.schema.assetPaths.safeParse(["/Game/MI_A", "/Game/MI_B"]).success).toBe(true);
    expect(materialTool.schema.overridesOnly.safeParse(true).success).toBe(true);
    expect(materialTool.actions.read_instance.description).toContain("assetPaths");
  });

  it("reads every path into instances[] and names the ones that failed", () => {
    const source = readHandlerFile("MaterialHandlers.cpp");
    expect(source).toContain('TryGetArrayParam(Params, TEXT("assetPaths"), AssetPaths)');
    expect(source).toContain('Result->SetArrayField(TEXT("instances"), Rows);');
    expect(source).toContain("WriteMaterialInstanceRead(Row, Material, !bOverridesOnly);");
    expect(source).toContain('{ TEXT("assetPaths") }');
  });
});
