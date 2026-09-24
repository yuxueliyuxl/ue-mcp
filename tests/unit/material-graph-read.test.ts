import { describe, expect, it } from "vitest";
import { parseParamsClause } from "../../src/action-schema.js";
import { materialTool } from "../../src/tools/material.js";

describe("material graph read surface", () => {
  it("publishes the graph read and optional expression-input schema", () => {
    expect(materialTool.schema.action.safeParse("read_graph").success).toBe(true);
    expect(materialTool.schema.includeInputs.safeParse(true).success).toBe(true);
    expect(materialTool.schema.includeInputs.safeParse(false).success).toBe(true);
    expect(materialTool.schema.includeInputs.safeParse(undefined).success).toBe(true);
    expect(materialTool.schema.includeInputs.safeParse("true").success).toBe(false);

    const readGraphParams = parseParamsClause(materialTool.actions.read_graph.description ?? "").map((p) => p.name);
    expect(readGraphParams, "read_graph documents expressionIndex").toContain("expressionIndex");
    expect(materialTool.schema.expressionIndex.safeParse(3).success).toBe(true);

    for (const action of ["list_expressions", "read_graph"]) {
      const params = parseParamsClause(materialTool.actions[action].description ?? "").map((p) => p.name);
      expect(params, `${action} documents cursor`).toContain("cursor");
      expect(params, `${action} documents limit`).toContain("limit");
    }
  });

  it("maps aliases and forwards graph paging options", () => {
    for (const alias of ["materialPath", "assetPath", "path"] as const) {
      expect(materialTool.actions.read_graph.mapParams?.({
        [alias]: "/Game/Materials/M_Master",
        cursor: "page-2",
        limit: 25,
        ignored: true,
      })).toEqual({
        materialPath: "/Game/Materials/M_Master",
        cursor: "page-2",
        limit: 25,
      });
    }

    expect(materialTool.actions.list_expressions.mapParams?.({
      assetPath: "/Game/Materials/M_Master",
      includeInputs: true,
      cursor: "page-3",
      limit: 10,
      ignored: true,
    })).toEqual({
      materialPath: "/Game/Materials/M_Master",
      includeInputs: true,
      cursor: "page-3",
      limit: 10,
    });

    expect(materialTool.actions.read_graph.mapParams?.({
      materialPath: "/Game/Materials/M_Master",
      expressionIndex: 7,
    })).toMatchObject({ materialPath: "/Game/Materials/M_Master", expressionIndex: 7 });

    expect(materialTool.actions.read_graph.mapParams?.({
      materialPath: "/Game/Materials/M_Canonical",
      assetPath: "/Game/Materials/M_Alias",
      path: "/Game/Materials/M_Legacy",
    })?.materialPath).toBe("/Game/Materials/M_Canonical");
  });
});
