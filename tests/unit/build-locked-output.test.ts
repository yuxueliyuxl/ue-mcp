// A link that fails because another process holds its output used to report
// only "Build failed with exit code 6". The holder is in the build output, so
// the failure names it and says what to do.
import { describe, expect, it } from "vitest";

import { describeLockedOutputs, findLockedOutputs } from "../../src/editor/editor-build.js";

const DLL = "C:\\Work\\Game\\Plugins\\X\\Binaries\\Win64\\UnrealEditor-X.dll";
const CRC = "C:\\UE\\Engine\\Binaries\\Win64\\CrashReportClientEditor.exe";

const UBA_LINE =
  `ERROR opening file ${DLL} for write (The process cannot access the file because it is being used by another ` +
  `process. - ${CRC})`;
const LNK_LINE = `LINK : fatal error LNK1104: cannot open file '${DLL}'`;

describe("outputs a build could not write", () => {
  it("reads the holder from the accelerator line and folds the linker line into it", () => {
    const locked = findLockedOutputs(["[1/3] Link UnrealEditor-X.dll", UBA_LINE, LNK_LINE, ""].join("\r\n"));
    expect(locked).toEqual([{ file: DLL, holder: CRC }]);
  });

  it("reads the holder when the line carries no closing parenthesis", () => {
    const line = `ERROR opening file ${DLL} for write - being used by another process. - ${CRC}`;
    expect(findLockedOutputs(line)).toEqual([{ file: DLL, holder: CRC }]);
  });

  it("keeps a linker-only failure, with no holder", () => {
    expect(findLockedOutputs(LNK_LINE)).toEqual([{ file: DLL, holder: null }]);
  });

  it("finds nothing in an ordinary compile error", () => {
    expect(findLockedOutputs("Foo.cpp(12): error C2065: 'Bar': undeclared identifier")).toEqual([]);
  });
});

describe("the message for a locked output", () => {
  it("names the crash reporter and points at stop_editor", () => {
    const message = describeLockedOutputs(6, [{ file: DLL, holder: CRC }]);
    expect(message).toContain("exit code 6");
    expect(message).toContain(DLL);
    expect(message).toContain(CRC);
    expect(message).toContain("crash reporter");
    expect(message).toContain("editor(action='stop_editor')");
  });

  it("names an editor holding the module", () => {
    const message = describeLockedOutputs(6, [{ file: DLL, holder: "C:\\UE\\Engine\\Binaries\\Win64\\UnrealEditor.exe" }]);
    expect(message).toContain("An editor still has the module loaded");
  });

  it("says the holder is unknown when the build did not name one", () => {
    const message = describeLockedOutputs(6, [{ file: DLL, holder: null }]);
    expect(message).toContain("another process the build did not name");
    expect(message).toContain("stop_editor");
  });

  it("tells somebody holding it with another program to close that", () => {
    const message = describeLockedOutputs(6, [{ file: DLL, holder: "C:\\Tools\\devenv.exe" }]);
    expect(message).toContain("Close that process");
  });
});
