import { describe, expect, it, vi } from "vitest";
import { editorTool } from "../../src/tools/editor.js";
import { classifyActionClass } from "../../src/action-class.js";
import type { ToolContext } from "../../src/types.js";

/**
 * #881: the playhead has to be movable to an exact frame, or a capture of the
 * evaluated world races realtime playback and lands wherever the tick left it.
 *
 * scrub_sequence is a separate action rather than a fourth sequenceAction verb:
 * that enum is closed at play|pause|stop and widening it is a contract change
 * on the transport, while a scrub carries a time the transport verbs have no
 * use for. The enum staying closed is asserted here so a later change has to
 * be deliberate.
 */
describe("editor.scrub_sequence", () => {
  it("forwards the sequence and the requested time", async () => {
    const call = vi.fn().mockResolvedValue({ success: true });
    const ctx = { bridge: { call } } as unknown as ToolContext;

    await editorTool.handler(ctx, {
      action: "scrub_sequence",
      sequencePath: "/Game/Cinematics/LS_Opening",
      frame: 48,
      timeUnit: "display",
    });

    expect(call).toHaveBeenCalledWith("scrub_sequence", {
      sequencePath: "/Game/Cinematics/LS_Opening",
      seconds: undefined,
      frame: 48,
      timeUnit: "display",
    }, undefined);
  });

  it("falls back to assetPath, the way the other sequencer actions do", async () => {
    const call = vi.fn().mockResolvedValue({ success: true });
    const ctx = { bridge: { call } } as unknown as ToolContext;

    await editorTool.handler(ctx, {
      action: "scrub_sequence",
      assetPath: "/Game/Cinematics/LS_Opening",
      seconds: 2.5,
    });

    expect(call).toHaveBeenCalledWith("scrub_sequence", {
      sequencePath: "/Game/Cinematics/LS_Opening",
      seconds: 2.5,
      frame: undefined,
      timeUnit: undefined,
    }, undefined);
  });

  it("accepts only the two time units the handler knows", () => {
    expect(editorTool.schema.timeUnit.safeParse("display").success).toBe(true);
    expect(editorTool.schema.timeUnit.safeParse("tick").success).toBe(true);
    expect(editorTool.schema.timeUnit.safeParse("frames").success).toBe(false);
  });

  it("leaves the play_sequence transport enum closed", () => {
    expect(editorTool.schema.sequenceAction.safeParse("play").success).toBe(true);
    expect(editorTool.schema.sequenceAction.safeParse("scrub").success).toBe(false);
  });

  it("classifies as a mutation, so it cannot land in an unnamed editor", () => {
    expect(classifyActionClass("editor", "scrub_sequence")).toEqual({
      class: "mutate",
      source: "lexicon",
    });
  });
});
