import { describe, expect, it } from "vitest";
import { niagaraTool } from "../../src/tools/niagara.js";
import { readHandlerFile } from "../../scripts/lib/cpp-registrations.mjs";

function spawnHandlerBody(): string {
  const source = readHandlerFile("NiagaraHandlers.cpp");
  const start = source.indexOf("FNiagaraHandlers::SpawnNiagaraAtLocation(");
  const end = source.indexOf("FNiagaraHandlers::SpawnNiagaraActor(", start);
  expect(start).toBeGreaterThan(0);
  expect(end).toBeGreaterThan(start);
  return source.slice(start, end);
}

describe("niagara.spawn host actor (#1396)", () => {
  it("spawns its own ANiagaraActor instead of parenting to WorldSettings", () => {
    const body = spawnHandlerBody();
    expect(body).not.toContain("UNiagaraFunctionLibrary::SpawnSystemAtLocation(");
    expect(body).toContain("SpawnActor<ANiagaraActor>");
  });

  it("rolls back by the created host's path, never by label", () => {
    const body = spawnHandlerBody();
    expect(body).toContain('Payload->SetStringField(TEXT("actorPath"), Host->GetPathName());');
    expect(body).not.toContain('Payload->SetStringField(TEXT("actorLabel")');
  });

  it("documents the host and the rollback", () => {
    const description = niagaraTool.actions.spawn.description ?? "";
    expect(description).toContain("NiagaraActor host");
    expect(description).toContain("actorPath");
  });
});
