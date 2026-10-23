/** #1384: a failed build names UnrealBuildTool's verdict and the first compiler errors. */
import { describe, expect, it } from "vitest";
import { describeBuildFailure, readBuildFailure } from "../../src/editor/editor-build.js";

// The tail of the UBT output from the failed Vale build, trimmed.
const VALE_UBT = [
  "Building ValeEditor...",
  "[1/4] Compile [x64] ValeJourneyCaravan.cpp",
  "C:\\Users\\david\\Projects\\UE\\Vale\\Plugins\\ValeJourney\\Source\\ValeJourney\\Private\\ValeJourneyCaravan.cpp(169,10): error C4458: declaration of 'PivotOffset' hides class member",
  "C:\\Program Files\\Epic Games\\UE_5.8\\Engine\\Source\\Runtime\\Engine\\Classes\\GameFramework\\Actor.h(812,10): note: see declaration of 'AActor::PivotOffset'",
  " Compile [x64] ValeJourneyCaravan.cpp: Exited with error code 2 . The build will fail.",
  "CompilationResultException: OtherCompilationError",
  "Result: Failed (OtherCompilationError)",
  "Total execution time: 186.21 seconds",
].join("\r\n");

describe("a failed build", () => {
  it("names UBT's verdict and the compiler errors, file and line first", () => {
    const f = readBuildFailure(VALE_UBT);
    expect(f.resultLine).toBe("Result: Failed (OtherCompilationError)");
    expect(f.errors).toEqual([
      "C:\\Users\\david\\Projects\\UE\\Vale\\Plugins\\ValeJourney\\Source\\ValeJourney\\Private\\ValeJourneyCaravan.cpp(169,10): error C4458: declaration of 'PivotOffset' hides class member",
    ]);
    expect(describeBuildFailure(6, VALE_UBT)).toBe(
      "Build failed with exit code 6 (Result: Failed (OtherCompilationError)). First errors: "
      + "C:\\Users\\david\\Projects\\UE\\Vale\\Plugins\\ValeJourney\\Source\\ValeJourney\\Private\\ValeJourneyCaravan.cpp(169,10): error C4458: declaration of 'PivotOffset' hides class member",
    );
  });

  it("names a rules failure, which stops UBT before any compile", () => {
    const out = "CompilationResultException: Could not find definition for module 'Foo', (referenced via Demo.uproject -> Bar.uplugin)\r\nResult: Failed (RulesError)";
    expect(describeBuildFailure(6, out)).toMatch(/\(Result: Failed \(RulesError\)\)\. First errors: CompilationResultException: Could not find definition for module 'Foo'/);
  });

  it("still says the exit code when the output names nothing", () => {
    expect(describeBuildFailure(1, "")).toBe("Build failed with exit code 1.");
  });
});

