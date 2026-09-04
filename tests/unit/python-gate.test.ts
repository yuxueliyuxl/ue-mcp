/**
 * The execute_python gate has to be passable (#960, #938).
 *
 * Two users reported the same wall independently. The refusal printed
 * `editor(invoke_function)` and then matched what came back against the bare
 * `invoke_function`, so sending back exactly what the tool had just printed did
 * not satisfy it. And because the candidate set is recomputed from the
 * taskSummary on every call, rewording the summary swapped in candidates that
 * had to be justified all over again: one reporter needed five rounds for a
 * single unchanged intent.
 *
 * This gate is the front door to the whole feedback pipeline, so "unusable"
 * means the gaps that Python papers over stop being recorded at all.
 */
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import {
  MIN_REASON_LENGTH,
  evaluateGate,
  gateRefusalMessage,
  knownRulings,
  parseRulings,
  resetRulings,
  ruledOutKey,
  type GateCandidate,
} from "../../src/python-gate.js";
import { editorTool } from "../../src/tools/editor.js";
import { resetAllWorkarounds } from "../../src/workaround-tracker.js";
import type { IBridge, ToolContext } from "../../src/types.js";

const CANDIDATES: GateCandidate[] = [
  { tool: "editor", action: "invoke_function", description: "Call a UFUNCTION", score: 9 },
  { tool: "asset", action: "set_property", description: "Set a property", score: 6 },
];

const REASON = "the function is on a component this call cannot reach";

beforeEach(() => {
  resetRulings();
  resetAllWorkarounds();
});
afterEach(() => {
  resetRulings();
  resetAllWorkarounds();
});

describe("ruledOutKey spellings (#938)", () => {
  it("reduces every spelling of an action reference to the bare name", () => {
    for (const spelling of [
      "invoke_function",
      "  invoke_function  ",
      "INVOKE_FUNCTION",
      "editor(invoke_function)",
      "editor( invoke_function )",
      "editor.invoke_function",
      "editor:invoke_function",
      "editor/invoke_function",
    ]) {
      expect(ruledOutKey(spelling), spelling).toBe("invoke_function");
    }
  });

  it("keeps distinct actions distinct", () => {
    expect(ruledOutKey("editor(execute_command)")).toBe("execute_command");
    expect(ruledOutKey("editor")).toBe("editor");
    expect(ruledOutKey("")).toBe("");
    expect(ruledOutKey(undefined)).toBe("");
  });
});

describe("parseRulings", () => {
  it("accepts a well-formed ruling under any spelling", () => {
    const { accepted, rejected } = parseRulings([
      { action: "editor(invoke_function)", reason: REASON },
      { action: "set_property", reason: REASON },
    ]);
    expect([...accepted.keys()].sort()).toEqual(["invoke_function", "set_property"]);
    expect(rejected).toEqual([]);
  });

  it("reports a too-short reason instead of dropping it in silence", () => {
    const { accepted, rejected } = parseRulings([{ action: "invoke_function", reason: "no" }]);
    expect(accepted.size).toBe(0);
    expect(rejected).toHaveLength(1);
    expect(rejected[0].entry).toBe("invoke_function");
    expect(rejected[0].why).toContain(String(MIN_REASON_LENGTH));
  });

  it("reports a bare string entry, which carries no reason", () => {
    const { accepted, rejected } = parseRulings(["editor(invoke_function)"]);
    expect(accepted.size).toBe(0);
    expect(rejected[0].why).toContain("no reason given");
  });

  it("tolerates a non-array ruledOut", () => {
    expect(parseRulings(undefined).accepted.size).toBe(0);
    expect(parseRulings("nope").accepted.size).toBe(0);
  });
});

describe("evaluateGate (#960)", () => {
  it("is satisfied by exactly the strings the refusal printed", () => {
    const first = evaluateGate(CANDIDATES, undefined);
    expect(first.unresolved).toHaveLength(2);

    // Feed the printed array straight back, filling in real reasons.
    const answer = first.ruledOutTemplate.map((entry) => ({ action: entry.action, reason: REASON }));
    expect(answer.map((a) => a.action)).toEqual(["editor(invoke_function)", "asset(set_property)"]);

    const second = evaluateGate(CANDIDATES, answer);
    expect(second.unresolved).toEqual([]);
  });

  it("remembers rulings so a reworded summary does not restart the round trip", () => {
    const ctx = { session: { key: "c:/proj/alpha" } };
    evaluateGate(CANDIDATES, [{ action: "editor(invoke_function)", reason: REASON }], ctx);

    // A reword pulls in a candidate that was not in the first search. Only the
    // new one is still outstanding; the earlier justification still counts.
    const reworded: GateCandidate[] = [
      CANDIDATES[0],
      { tool: "level", action: "spawn_actor", description: "Spawn", score: 5 },
    ];
    const verdict = evaluateGate(reworded, undefined, ctx);
    expect(verdict.unresolved.map((c) => c.action)).toEqual(["spawn_actor"]);
    expect(verdict.satisfied).toEqual(["editor(invoke_function)"]);
  });

  it("converges: each round only ever adds the genuinely new candidate", () => {
    const ctx = { session: { key: "c:/proj/converge" } };
    let pool: GateCandidate[] = [];
    let rounds = 0;
    for (const extra of ["a_one", "a_two", "a_three"]) {
      pool = [...pool, { tool: "editor", action: extra, score: 5 }];
      const verdict = evaluateGate(pool, undefined, ctx);
      expect(verdict.unresolved.map((c) => c.action)).toEqual([extra]);
      evaluateGate(
        pool,
        verdict.ruledOutTemplate.map((e) => ({ action: e.action, reason: REASON })),
        ctx,
      );
      rounds++;
    }
    expect(rounds).toBe(3);
    expect(evaluateGate(pool, undefined, ctx).unresolved).toEqual([]);
  });

  it("keeps one editor's rulings out of another's gate", () => {
    const a = { session: { key: "c:/proj/alpha" } };
    const b = { session: { key: "c:/proj/beta" } };
    evaluateGate(CANDIDATES, [{ action: "invoke_function", reason: REASON }], a);
    expect(knownRulings(a).has("invoke_function")).toBe(true);
    expect(knownRulings(b).has("invoke_function")).toBe(false);
    expect(evaluateGate(CANDIDATES, undefined, b).unresolved).toHaveLength(2);
  });
});

describe("gateRefusalMessage", () => {
  it("states the accepted spellings and shows the array to send", () => {
    const verdict = evaluateGate(CANDIDATES, undefined);
    const message = gateRefusalMessage("read a component property", CANDIDATES, verdict);
    expect(message).toContain("editor(invoke_function)");
    expect(message).toContain("editor.invoke_function");
    expect(message).toContain('"ruledOut"');
    expect(message).toContain(String(MIN_REASON_LENGTH));
  });

  it("names the entries that did not count", () => {
    const verdict = evaluateGate(CANDIDATES, [{ action: "invoke_function", reason: "no" }]);
    const message = gateRefusalMessage("read a component property", CANDIDATES, verdict);
    expect(message).toContain("did not count");
    expect(message).toContain("invoke_function");
  });
});

describe("editor.execute_python end to end", () => {
  const calls: Array<{ method: string; params?: Record<string, unknown> }> = [];
  const bridge = {
    isConnected: true,
    call: async (method: string, params?: Record<string, unknown>) => {
      calls.push({ method, params });
      return { success: true };
    },
    connect: async () => undefined,
    retargetProject: () => ({ projectPath: null, port: 0, portSource: "default", verified: false }),
    getTarget: () => ({ projectPath: null, port: 0, portSource: "default", verified: false }),
  } as unknown as IBridge;

  const ctx = {
    bridge,
    project: {} as ToolContext["project"],
    session: { key: "c:/proj/e2e" },
  } as unknown as ToolContext;

  const run = (params: Record<string, unknown>): Promise<Record<string, unknown>> =>
    editorTool.actions.execute_python.handler!(ctx, params) as Promise<Record<string, unknown>>;

  beforeEach(() => {
    calls.length = 0;
  });

  it("accepts the strings it printed, on the very next call", async () => {
    const summary = "invoke a ufunction on an actor component";
    const blocked = await run({ code: "print(1)", taskSummary: summary });
    expect(blocked.blocked).toBe(true);

    // The exact array the refusal handed over, with real reasons filled in.
    const template = (blocked.sendThisBack as { ruledOut: Array<{ action: string }> }).ruledOut;
    expect(template.length).toBeGreaterThan(0);
    const answer = template.map((entry) => ({ action: entry.action, reason: REASON }));

    const passed = await run({ code: "print(1)", taskSummary: summary, ruledOut: answer });
    expect(passed.blocked).toBeUndefined();
    expect(calls.map((c) => c.method)).toContain("execute_python");
  });

  it("also accepts the bare spelling the refusal used to demand", async () => {
    const summary = "invoke a ufunction on an actor component";
    const blocked = await run({ code: "print(1)", taskSummary: summary });
    const bare = (blocked.needReasonFor as string[]).map((qualified) => ({
      action: ruledOutKey(qualified),
      reason: REASON,
    }));
    const passed = await run({ code: "print(1)", taskSummary: summary, ruledOut: bare });
    expect(passed.blocked).toBeUndefined();
  });

  it("does not re-ask for a candidate already justified under a different summary", async () => {
    const blocked = await run({ code: "print(1)", taskSummary: "invoke a ufunction on an actor component" });
    const answer = (blocked.sendThisBack as { ruledOut: Array<{ action: string }> }).ruledOut
      .map((entry) => ({ action: entry.action, reason: REASON }));
    await run({ code: "print(1)", taskSummary: "invoke a ufunction on an actor component", ruledOut: answer });

    // Same intent, different words. Anything still outstanding must be a
    // candidate the first search never returned.
    const reworded = await run({ code: "print(1)", taskSummary: "call a ufunction on an actor component" });
    if (reworded.blocked) {
      const outstanding = (reworded.needReasonFor as string[]).map(ruledOutKey);
      const alreadyDone = new Set(
        (blocked.needReasonFor as string[]).map(ruledOutKey),
      );
      for (const action of outstanding) expect(alreadyDone.has(action)).toBe(false);
    }
  });

  it("still refuses a call with no taskSummary at all", async () => {
    const refused = await run({ code: "print(1)" });
    expect(refused.blocked).toBe(true);
    expect(refused.reason).toBe("missing_task_summary");
    expect(calls).toEqual([]);
  });
});
