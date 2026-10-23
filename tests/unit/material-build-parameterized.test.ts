import { describe, expect, it } from "vitest";
import { materialTool } from "../../src/tools/material.js";
import { readHandlerFile } from "../../scripts/lib/cpp-registrations.mjs";

describe("build_material parameters, math nodes and instances (#1229)", () => {
  it("accepts a parameterized master with per-piece instances", () => {
    const schema = materialTool.schema;
    expect(schema.textureParameters.safeParse(true).success).toBe(true);
    expect(schema.textureParameters.safeParse({ orm: "ORM" }).success).toBe(true);
    expect(schema.parameters.safeParse([
      { name: "Tint", type: "vector", default: { r: 0.25, g: 0.22, b: 0.15, a: 1 } },
      { name: "AOStrength", type: "scalar", default: 0.85 },
    ]).success).toBe(true);
    expect(schema.nodes.safeParse([
      { name: "One", class: "Constant", value: 1 },
      { name: "AOLerp", class: "LinearInterpolate" },
    ]).success).toBe(true);
    expect(schema.instances.safeParse([
      { name: "MI_Boots", parameters: { Tint: { r: 0.03, g: 0.03, b: 0.02 } }, assignToMesh: "/Game/SKM_Outfit", slotName: "Boots" },
    ]).success).toBe(true);
    expect(materialTool.actions.build_material.description).toContain("instances[]");
  });

  it("builds parameters and instances natively and routes math through build_material_graph", () => {
    const source = readHandlerFile("MaterialHandlers_Build.cpp");
    expect(source).toContain("UMaterialExpressionTextureSampleParameter2D::StaticClass()");
    expect(source).toContain("UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, ParamName, Color)");
    expect(source).toContain("BuildMaterialGraph(GraphParams)");
    expect(source).toContain("MaterialBuildAssignToMesh(InstanceMesh, SlotNames, SlotIndices, Instance)");
  });
});
