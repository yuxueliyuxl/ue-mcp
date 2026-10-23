import { describe, expect, it } from "vitest";
import { assetTool } from "../../src/tools/asset.js";
import { handlerSpecs } from "../../src/tools/specs/asset.generated.js";

describe("asset(bulk_rename) typed items (#1291)", () => {
  it("names every field a renames[] item may carry", () => {
    const fields = (handlerSpecs.bulk_rename_assets.params[0].fields ?? []).map((f) => f.name);
    expect(fields).toEqual(["sourcePath", "destinationPath", "assetPath", "newPackagePath", "newName"]);
  });

  it("refuses an undeclared key instead of stripping it", () => {
    const renames = assetTool.schema.renames;
    expect(renames.safeParse([{ sourcePath: "/Game/A", destinationPath: "/Game/B/A" }]).success).toBe(true);
    expect(renames.safeParse([{ assetPath: "/Game/A", newName: "B" }]).success).toBe(true);
    expect(renames.safeParse([{ sourcePath: "/Game/A", destinationPath: "/Game/B", newFolder: "X" }]).success).toBe(false);
  });
});
