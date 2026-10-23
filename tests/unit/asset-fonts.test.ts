import { describe, expect, it } from "vitest";
import { assetTool } from "../../src/tools/asset.js";
import { readHandlerFile } from "../../scripts/lib/cpp-registrations.mjs";

describe("asset font import and composite fonts (#1377)", () => {
  it("routes import_font and create_font to their bridge handlers", () => {
    expect(assetTool.actions.import_font).toMatchObject({ bridge: "import_font" });
    expect(assetTool.actions.create_font).toMatchObject({ bridge: "create_font" });
    expect(assetTool.schema.fontFaces.safeParse(["/Game/Fonts/Inter-Regular", "/Game/Fonts/Inter-Bold"]).success).toBe(true);
    expect(assetTool.schema.typefaceNames.safeParse(["Regular", "Bold"]).success).toBe(true);
  });

  it("writes the face bytes without the import dialog and fills the composite font", () => {
    const source = readHandlerFile("AssetHandlers_Font.cpp");
    expect(source).toContain("Face->FontFaceData->SetData(MoveTemp(Bytes));");
    expect(source).toContain("Face->CacheSubFaces();");
    expect(source).toContain("Entry.Font = FFontData(Faces[i]);");
    expect(source).toContain("Font->FontCacheType = EFontCacheType::Runtime;");
  });
});
