import { describe, expect, it } from "vitest";
import { materialTool } from "../../src/tools/material.js";
import { gameplayTool } from "../../src/tools/gameplay.js";
import { readHandlerFile } from "../../scripts/lib/cpp-registrations.mjs";

describe("material graph authoring gaps (#1403)", () => {
  it("documents the build_graph node, connection and property spec in the schema", () => {
    const nodes = materialTool.schema.nodes.safeParse([
      { name: "Wave", class: "Sine", posX: -300 },
      { name: "Tint", class: "VectorParameter", parameterName: "Tint", defaultValue: { r: 1, g: 0, b: 0, a: 1 } },
      { name: "Mul", class: "Multiply" },
    ]);
    expect(nodes.success).toBe(true);
    expect(materialTool.schema.nodes.safeParse([{ name: "NoClass" }]).success).toBe(false);
    expect(materialTool.schema.connections.safeParse([
      { from: "Wave", to: "Mul", toInput: "A" },
      { from: "Tint", fromOutput: 0, to: "Mul", toInput: "B" },
    ]).success).toBe(true);
    expect(materialTool.schema.propertyConnections.safeParse([{ property: "BaseColor", from: "Mul" }]).success).toBe(true);
    const description = materialTool.actions.build_graph.description ?? "";
    expect(description).toContain("connections[]");
    expect(description).toContain("toInput Input");
  });

  it("routes delete_orphans to the native handler", () => {
    expect(materialTool.actions.delete_orphans).toMatchObject({ bridge: "delete_orphan_expressions" });
    const source = readHandlerFile("MaterialHandlers_WholeMaterial.cpp");
    expect(source).toContain("FMaterialHandlers::DeleteOrphanExpressions(");
    // validate and delete_orphans share one reachability walk, so they agree.
    expect(source).toContain("const TSet<UMaterialExpression*> Referenced = CollectReachableExpressions(Material);");
  });

  it("addresses a single unnamed input as Input or by index", () => {
    const source = readHandlerFile("MaterialHandlers_Graph.cpp");
    expect(source).toContain('Trimmed.Equals(TEXT("Input"), ESearchCase::IgnoreCase)');
    expect(source).toContain("UMaterialGraphNode::GetShortenPinName(Name)");
    expect(materialTool.actions.connect_expressions.description).toContain("targetInput Input");
  });

  it("reads IMC trigger and modifier properties", () => {
    const source = readHandlerFile("GameplayHandlers_Input.cpp");
    expect(source).toContain("InputDepth_Internal::DescribeHelper(Mapping.Modifiers[Index], Index)");
    expect(gameplayTool.actions.read_imc.description).toContain("SwizzleAxis Order");
  });
});
