/**
 * The beacon flow sets material constants by expression index.
 *
 * set_expression_value addresses a node by its position in the material's
 * expression list, and set_base_color appends a Constant3Vector of its own. A
 * literal index written against the add_expression calls alone therefore lands
 * on the base colour node: M_Pillar's Metallic write (index 0) hit the
 * Constant3Vector, and its Roughness write (index 1) hit Metallic.
 *
 * The flow runs here through the real runner and every real category tool, on
 * a bridge that keeps each material's expression list the way the plugin does,
 * so a step whose options break an action's parameter contract fails here too.
 */
import { describe, expect, it } from "vitest";
import { ALL_TOOLS } from "../../src/tools.js";
import { buildDefaults } from "../../src/flow/loader.js";
import { FlowConfigSchema, type FlowConfig } from "../../src/flow/schema.js";
import { buildFlowRegistry } from "../../src/flow/registry.js";
import { createFlowTool } from "../../src/flow/flow-tool.js";
import { ProjectContext } from "../../src/config/project.js";
import type { IBridge } from "../../src/bridge/bridge.js";

interface Node { kind: string; name: string; value?: unknown }

/** Keeps per-material expression lists in plugin order: appended, never reordered. */
function materialBridge(): { bridge: IBridge; graphs: Map<string, Node[]> } {
  const graphs = new Map<string, Node[]>();
  const graph = (p: Record<string, unknown>) => {
    const key = String(p.materialPath ?? p.assetPath ?? p.path);
    const list = graphs.get(key);
    if (!list) throw new Error(`no material at ${key}`);
    return list;
  };
  const call = async (method: string, params: Record<string, unknown> = {}) => {
    switch (method) {
      case "create_material": {
        const path = `${String(params.packagePath)}/${String(params.name)}`;
        graphs.set(path, []);
        return { success: true, path };
      }
      case "set_material_base_color":
        graph(params).push({ kind: "Constant3Vector", name: "BaseColor", value: params.color });
        return { success: true };
      case "add_material_expression": {
        const list = graph(params);
        list.push({ kind: String(params.expressionType), name: String(params.name) });
        return { success: true, expressionIndex: list.length - 1, nodeId: String(list.length - 1) };
      }
      case "set_expression_value": {
        const list = graph(params);
        const index = Number(params.expressionIndex);
        if (!Number.isInteger(index) || index < 0 || index >= list.length) {
          return { success: false, error: `Expression index ${String(params.expressionIndex)} out of range` };
        }
        list[index].value = params.value;
        return { success: true };
      }
      default:
        return { success: true };
    }
  };
  const bridge: IBridge = {
    isConnected: true,
    connect: async () => {},
    retargetProject: () => ({ projectPath: null, port: 0, portSource: "default" as const, verified: true }),
    getTarget: () => ({ projectPath: null, port: 0, portSource: "default" as const, verified: true }),
    call,
  };
  return { bridge, graphs };
}

function beaconConfig(): FlowConfig {
  const defaults = buildDefaults(ALL_TOOLS) as { flows: Record<string, unknown> };
  return FlowConfigSchema.parse({ "ue-mcp": { version: 1 }, flows: { beacon: defaults.flows.beacon } });
}

const node = (list: Node[] | undefined, name: string) => list?.find((n) => n.name === name);

describe("beacon material constants", () => {
  it("land on the node each add_expression step created", async () => {
    const { bridge, graphs } = materialBridge();
    const flowTool = createFlowTool(buildFlowRegistry(ALL_TOOLS), () => beaconConfig());
    const run = (await flowTool.handler(
      { bridge, project: new ProjectContext() },
      { action: "run", flowName: "beacon" },
    )) as { success: boolean };
    expect(run.success, JSON.stringify(run).slice(0, 2000)).toBe(true);

    const pillar = graphs.get("/Game/Flows/Beacon/M_Pillar");
    expect(node(pillar, "BaseColor")?.value).toEqual({ r: 60, g: 65, b: 80 });
    expect(node(pillar, "Metallic")?.value).toBe(1.0);
    expect(node(pillar, "Roughness")?.value).toBe(0.3);

    const glow = graphs.get("/Game/Flows/Beacon/M_Glow");
    expect(node(glow, "GlowStrength")?.value).toBe(50);
    expect(node(glow, "GlowColor")?.value).toBeUndefined();
  });

  it("never name a node by a literal index", () => {
    const steps = (beaconConfig().flows.beacon as { steps: Record<string, { task: string; options?: Record<string, unknown> }> }).steps;
    const literal = Object.entries(steps)
      .filter(([, s]) => s.task === "material.set_expression_value" && typeof s.options?.expressionIndex === "number")
      .map(([n]) => n);
    expect(literal).toEqual([]);
  });
});
