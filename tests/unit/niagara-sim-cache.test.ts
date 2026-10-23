import { describe, expect, it } from "vitest";
import { niagaraTool } from "../../src/tools/niagara.js";
import { readHandlerFile } from "../../scripts/lib/cpp-registrations.mjs";

describe("niagara sim cache readback (#1394)", () => {
  it("routes capture and read to their bridge handlers", () => {
    expect(niagaraTool.actions.capture_sim_cache).toMatchObject({ bridge: "capture_niagara_sim_cache" });
    expect(niagaraTool.actions.read_sim_cache).toMatchObject({ bridge: "read_niagara_sim_cache" });
  });

  it("declares the read parameters from the C++ spec", () => {
    const parsed = niagaraTool.schema.attributes.safeParse(["Particles.Position", "Age"]);
    expect(parsed.success).toBe(true);
    expect(niagaraTool.schema.simCachePath.safeParse("/Engine/Transient.MCPSimCache_0").success).toBe(true);
  });

  it("accepts qualified names and tells a missing name from an empty frame", () => {
    const source = readHandlerFile("NiagaraHandlers_SimCache.cpp");
    expect(source).toContain("Bare.FindLastChar(TEXT('.'), Dot)");
    expect(source).toContain('Entry->SetBoolField(TEXT("found"), Attr != nullptr);');
    expect(source).toContain('Result->SetNumberField(TEXT("numInstances"), NumInstances);');
    expect(source).toContain('Result->SetBoolField(TEXT("success"), false);');
  });

  it("documents both traps in the action descriptions", () => {
    expect(niagaraTool.actions.read_sim_cache.description).toContain("Particles.Age");
    expect(niagaraTool.actions.read_sim_cache.description).toContain("count 0");
    expect(niagaraTool.actions.capture_sim_cache.description).toContain("numInstancesPerFrame");
  });
});
