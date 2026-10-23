/**
 * Every action a hint names exists (#1260).
 *
 * An error that tells the caller to run editor(action="stop_pie") sends them to
 * UNKNOWN_ACTION. Hints in the plugin's C++ strings and in the server's own
 * strings are scanned for `category(action...)` and `category(action="...")`
 * and each one is resolved against the advertised categories.
 */
import * as fs from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";
import { ALL_TOOLS } from "../../src/tools.js";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");

const ACTIONS = new Map<string, Set<string>>(
  ALL_TOOLS.map((tool) => [tool.name, new Set(Object.keys(tool.actions))]),
);

/** Calls the flow and gateway tools name, which are not categories. */
const OTHER_TOOLS = new Set(["flow", "tools"]);

function filesUnder(dir: string, pattern: RegExp): string[] {
  const out: string[] = [];
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) out.push(...filesUnder(full, pattern));
    else if (pattern.test(entry.name)) out.push(full);
  }
  return out;
}

/**
 * `category(name` where name is an action-shaped word: two characters or
 * more, ending in a letter or digit, and not a parameter (`name=`, `name:`).
 * Prose like `asset(s)` and wildcards like `audio(metasound_*)` do not match.
 */
const HINT = new RegExp(
  String.raw`\b(${[...ACTIONS.keys()].join("|")})\((?:action\s*=\s*\\?["'])?([a-z][a-z0-9_]*[a-z0-9])(?![A-Za-z0-9_*.]|\s*[=:(])`,
  "g",
);

function unknownHints(file: string, lineFilter: (line: string) => boolean): string[] {
  const problems: string[] = [];
  fs.readFileSync(file, "utf8").split(/\r?\n/).forEach((line, index) => {
    if (!lineFilter(line)) return;
    for (const match of line.matchAll(HINT)) {
      const [, category, action] = match;
      if (OTHER_TOOLS.has(category)) continue;
      if (!ACTIONS.get(category)?.has(action)) {
        problems.push(`${path.relative(ROOT, file)}:${index + 1} ${category}(${action})`);
      }
    }
  });
  return problems;
}

describe("hints name actions that exist", () => {
  it("in the plugin's C++ strings", () => {
    const files = filesUnder(path.join(ROOT, "plugin", "ue_mcp_bridge", "Source"), /\.(h|cpp)$/);
    expect(files.length).toBeGreaterThan(100);
    const problems = files.flatMap((file) => unknownHints(file, (line) => line.includes("TEXT(")));
    expect(problems).toEqual([]);
  });

  it("in the server's own strings", () => {
    const files = filesUnder(path.join(ROOT, "src"), /\.ts$/);
    const problems = files.flatMap((file) => unknownHints(file, () => true));
    expect(problems).toEqual([]);
  });

  it("catches the hint that started it", () => {
    const line = 'TEXT("Stop play with editor(action=\\"stop_pie\\") and retry.")';
    expect(line.match(HINT)?.[0]).toBe('editor(action=\\"stop_pie');
    expect(ACTIONS.get("editor")?.has("stop_pie")).toBe(false);
    expect(ACTIONS.get("editor")?.has("play_in_editor")).toBe(true);
  });

  it("ignores prose and parameters that look like calls", () => {
    for (const prose of ["asset(s)", "widget(s)", "audio(metasound_*)", "editor(pieAction=\"stop\")", "plugins(req.method)"]) {
      expect(prose.match(HINT), prose).toBeNull();
    }
  });
});
