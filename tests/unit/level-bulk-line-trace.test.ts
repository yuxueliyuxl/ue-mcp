import { describe, expect, it } from "vitest";
import { classifyActionClass } from "../../src/action-class.js";
import { levelTool } from "../../src/tools/level.js";

describe("level.bulk_line_trace", () => {
  it("is exposed through the level action schema", () => {
    expect(levelTool.schema.action.safeParse("bulk_line_trace").success).toBe(true);
  });

  it("accepts a bounded traces batch with the same fields as line_trace", () => {
    const traces = [
      { start: { x: 0, y: 0, z: 100 }, end: { x: 0, y: 0, z: 0 } },
      {
        start: { x: 0, y: 0, z: 100 },
        direction: { x: 0, y: 0, z: -1 },
        distance: 200000,
        traceComplex: true,
        channel: "Visibility",
        ignoreActors: ["Player"],
      },
    ];
    expect(levelTool.schema.traces.safeParse(traces).success).toBe(true);
  });

  it("rejects empty, oversized, and malformed batches", () => {
    expect(levelTool.schema.traces.safeParse([]).success).toBe(false);
    expect(levelTool.schema.traces.safeParse(
      Array.from({ length: 257 }, () => ({ start: { x: 0, y: 0, z: 0 }, end: { x: 0, y: 0, z: 1 } })),
    ).success).toBe(false);
    expect(levelTool.schema.traces.safeParse([{ start: "origin" }]).success).toBe(false);
    expect(levelTool.schema.traces.safeParse([{ end: { x: 0, y: 0, z: 1 } }]).success).toBe(false);
  });

  it("routes only traces to the native bridge", () => {
    const action = levelTool.actions.bulk_line_trace;
    expect(action.bridge).toBe("bulk_line_trace");
    const traces = [{ start: { x: 0, y: 0, z: 0 }, end: { x: 1, y: 0, z: 0 } }];
    expect(action.mapParams?.({
      action: "bulk_line_trace",
      traces,
      unrelated: "ignored",
    })).toEqual({ traces });
  });

  it("documents the cap and ordered per-item contract", () => {
    const description = levelTool.actions.bulk_line_trace.description ?? "";
    expect(description).toContain("traces");
    expect(description).toContain("256");
    expect(description).toMatch(/order/);
  });

  it("is a read, like line_trace", () => {
    expect(classifyActionClass("level", "bulk_line_trace")).toEqual({
      class: "read",
      source: "override",
    });
  });
});
