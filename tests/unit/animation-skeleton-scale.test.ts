import { describe, expect, it } from "vitest";
import { animationTool } from "../../src/tools/animation.js";
import { assetTool } from "../../src/tools/asset.js";
import { handlerSpecs as animationSpecs } from "../../src/tools/specs/animation.generated.js";
import { handlerSpecs as assetSpecs } from "../../src/tools/specs/asset.generated.js";
import { readHandlerFile } from "../../scripts/lib/cpp-registrations.mjs";

describe("skeleton scale normalisation and bone listing (#1201)", () => {
  it("edit_skeleton_bones offers normalize_scale", () => {
    const edits = animationSpecs.edit_skeleton_bones.params.find((p) => p.name === "edits");
    expect(edits?.description).toContain("{op:'normalize_scale',bone}");
    expect(animationTool.actions.edit_skeleton_bones.description).toContain("maxComponentDrift");
    const source = readHandlerFile("AnimationHandlers_Skeleton.cpp");
    expect(source).toContain("FTransform(Global.GetRotation(), Global.GetTranslation(), FVector::OneVector)");
    expect(source).toContain("NewGlobal[Bone].GetRelativeTransform(ParentGlobal ? *ParentGlobal : OuterGlobal)");
  });

  it("list_skeleton_bones honours limit and offset and summarises scale", () => {
    expect(assetSpecs.list_skeleton_bones.params.map((p) => p.name)).toEqual(["assetPath", "includeTransforms", "limit", "offset", "includeBones"]);
    expect(assetTool.actions.list_skeleton_bones.description).toContain("scaleSummary");
    const source = readHandlerFile("AssetHandlers_Mesh.cpp");
    expect(source).toContain('Result->SetObjectField(TEXT("scaleSummary"), Summary)');
    expect(source).toContain("const int32 Last = bHasLimit ? FMath::Min(NumBones, First + Limit) : NumBones;");
  });
});
