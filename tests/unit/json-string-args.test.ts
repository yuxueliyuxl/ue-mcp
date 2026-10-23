import { describe, expect, it } from "vitest";
import { coerceJsonStringArgs, validateCategoryParams } from "../../src/surface/context/call-envelope.js";
import { levelTool } from "../../src/tools/level.js";
import { editorTool } from "../../src/tools/editor.js";

// Some clients JSON-encode object arguments; place_actor's location arrived
// as a string and was refused as "Expected object, received string" (#1370).
describe("JSON-encoded object arguments (#1370)", () => {
  it("decodes a string where the shape takes only an object", () => {
    const out = coerceJsonStringArgs(levelTool, { action: "place_actor", actorClass: "ExponentialHeightFog", location: '{"x":1,"y":2,"z":3}' });
    expect(out.location).toEqual({ x: 1, y: 2, z: 3 });
    expect(out.actorClass).toBe("ExponentialHeightFog");
  });

  it("passes validation once decoded", () => {
    const parsed = validateCategoryParams(levelTool, { action: "place_actor", actorClass: "ExponentialHeightFog", location: ' {"x":0,"y":0,"z":100}' });
    expect(parsed.location).toEqual({ x: 0, y: 0, z: 100 });
  });

  it("leaves a string that does not parse for validation to refuse", () => {
    expect(() => validateCategoryParams(levelTool, { action: "place_actor", actorClass: "A", location: "{x:1}" })).toThrow(/location/);
  });

  it("never rewrites a parameter that takes any value", () => {
    const value = '{"instanceClass":"RootMotionModifier_Scale"}';
    const out = coerceJsonStringArgs(editorTool, { action: "set_property", objectPath: "/Game/M.M", propertyName: "P", value });
    expect(out.value).toBe(value);
  });

  it("returns the same bag when nothing needed decoding", () => {
    const bag = { action: "place_actor", actorClass: "A", location: { x: 0, y: 0, z: 0 } };
    expect(coerceJsonStringArgs(levelTool, bag)).toBe(bag);
  });
});
