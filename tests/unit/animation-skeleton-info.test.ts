import { describe, expect, it } from "vitest";
import { animationTool } from "../../src/tools/animation.js";
import { blueprintTool } from "../../src/tools/blueprint.js";
import { handlerSpecs } from "../../src/tools/specs/animation.generated.js";
import { readHandlerFile } from "../../scripts/lib/cpp-registrations.mjs";

describe("skeleton reads and virtual bone names (#1238)", () => {
  it("get_skeleton_info takes a Skeleton as well as a SkeletalMesh", () => {
    expect(handlerSpecs.get_skeleton_info.params[0].description).toContain("Skeleton");
    expect(animationTool.actions.get_skeleton_info.description).toContain("virtualBones");
    const source = readHandlerFile("AnimationHandlers.cpp");
    expect(source).toContain("USkeleton* Skeleton = SkeletalMesh ? SkeletalMesh->GetSkeleton() : Cast<USkeleton>(LoadedAsset)");
    expect(source).toContain("Skeleton->GetSlotGroups()");
  });

  it("add_virtual_bone takes a name and remove_virtual_bone's rollback keeps it", () => {
    expect(handlerSpecs.add_virtual_bone.params.map((p) => p.name)).toEqual(["skeletonPath", "sourceBone", "targetBone", "name"]);
    const source = readHandlerFile("AnimationHandlers.cpp");
    expect(source).toContain("Skeleton->RenameVirtualBone(NewBoneName, FName(*RequestedName))");
    expect(source).toContain('Rollback->SetStringField(TEXT("name"), BoneName)');
  });

  it("reparent_blueprint reports variable defaults the rebuild changed", () => {
    expect(blueprintTool.actions.reparent.description).toContain("variableDefaultsChanged");
    const source = readHandlerFile("BlueprintHandlers.cpp");
    expect(source).toContain('Result->SetArrayField(TEXT("variableDefaultsChanged"), DefaultsReset)');
    expect(source.indexOf("const TMap<FName, FString> DefaultsBefore = ReadVariableDefaults()"))
      .toBeLessThan(source.indexOf("UBlueprintEditorLibrary::ReparentBlueprint(Blueprint, NewParent)"));
  });
});
