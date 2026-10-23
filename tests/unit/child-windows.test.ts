/**
 * Every child process ue-mcp starts passes windowsHide. The daemon has no
 * console, so on Windows a child started without it, a build through cmd or a
 * git call during a flow, opens a terminal window that stays until it exits.
 */
import * as fs from "node:fs";
import * as path from "node:path";
import { describe, expect, it } from "vitest";

const SRC = path.resolve(__dirname, "../../src");

function calls(): Array<{ where: string; call: string }> {
  const found: Array<{ where: string; call: string }> = [];
  const walk = (dir: string) => {
    for (const name of fs.readdirSync(dir)) {
      const p = path.join(dir, name);
      if (fs.statSync(p).isDirectory()) walk(p);
      else if (p.endsWith(".ts")) scan(p);
    }
  };
  const scan = (file: string) => {
    const src = fs.readFileSync(file, "utf-8");
    if (!/["']node:child_process["']|["']child_process["']/.test(src)) return;
    const re = /\b(execSync|execFileSync|spawnSync|execFile|spawn|exec)\(/g;
    for (let m = re.exec(src); m; m = re.exec(src)) {
      // A method such as regex.exec( is not a child process.
      if (src[m.index - 1] === ".") continue;
      let depth = 0;
      let end = m.index + m[0].length - 1;
      for (; end < src.length; end++) {
        if (src[end] === "(") depth++;
        else if (src[end] === ")" && --depth === 0) break;
      }
      const line = src.slice(0, m.index).split("\n").length;
      found.push({ where: `${path.relative(SRC, file).replace(/\\/g, "/")}:${line}`, call: src.slice(m.index, end + 1) });
    }
  };
  walk(SRC);
  return found;
}

describe("child processes", () => {
  it("finds the calls it checks", () => {
    expect(calls().length).toBeGreaterThan(10);
  });

  it("start hidden on Windows, so no terminal window opens", () => {
    const shown = calls().filter((c) => !/windowsHide/.test(c.call)).map((c) => `${c.where}  ${c.call.split("\n")[0]}`);
    expect(shown).toEqual([]);
  });
});
