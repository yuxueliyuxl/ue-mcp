/**
 * Which UI bundle the daemon serves (#1618): a bundle named for a loaded
 * extension is served at that extension's version, or else the newest one the
 * extension declares compatible, and never one newer than the extension.
 */
import { describe, expect, it } from "vitest";
import * as crypto from "node:crypto";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { selectBundle, signBundle, UiBundleStore, type UiManifest } from "../../src/daemon/ui-bundles.js";
import { DAEMON_API_VERSION } from "../../src/daemon/discovery.js";

const b = (name: string, version: string) => ({ manifest: { name, version, api: { min: 1, max: 1 }, files: {} } as UiManifest });
const versions = (xs: Array<{ manifest: UiManifest }> | null) => xs?.map((x) => `${x.manifest.name}@${x.manifest.version}`);
const pick = (cs: ReturnType<typeof b>[], owners: Parameters<typeof selectBundle>[1]) => {
  const s = selectBundle(cs, owners);
  return s ? `${s.manifest.name}@${s.manifest.version}` : null;
};

describe("selectBundle", () => {
  const installed = [b("studio", "0.1.23"), b("studio", "0.1.24-beta.3"), b("studio", "0.1.24-beta.19"), b("studio", "0.1.22")];

  it("serves the newest bundle when no extension is loaded", () => {
    expect(pick(installed, [])).toBe("studio@0.1.24-beta.19");
  });

  it("serves the bundle at the loaded extension's version, not a newer one", () => {
    expect(pick(installed, [{ name: "studio", version: "0.1.23" }])).toBe("studio@0.1.23");
    expect(pick(installed, [{ name: "studio", version: "0.1.24-beta.3" }])).toBe("studio@0.1.24-beta.3");
  });

  it("without an exact match, serves the newest older bundle", () => {
    expect(pick(installed, [{ name: "studio", version: "0.1.24-beta.10" }])).toBe("studio@0.1.24-beta.3");
  });

  it("never serves a bundle older than the extension's declared minimum", () => {
    expect(pick(installed, [{ name: "studio", version: "0.1.24-beta.10", uiMin: "0.1.24-beta.1" }])).toBe("studio@0.1.24-beta.3");
    expect(pick(installed, [{ name: "studio", version: "0.1.24-beta.2", uiMin: "0.1.24-beta.1" }])).toBeNull();
  });

  it("serves nothing rather than a newer bundle when every one of its bundles is newer", () => {
    expect(pick(installed, [{ name: "studio", version: "0.1.21" }])).toBeNull();
  });

  it("ranks a release above its own prereleases within the extension's range", () => {
    const cs = [b("studio", "0.1.24-beta.19"), b("studio", "0.1.24"), b("studio", "0.1.25-beta.1")];
    expect(pick(cs, [{ name: "studio", version: "0.1.24" }])).toBe("studio@0.1.24");
  });

  it("prefers the extension's own bundles and leaves other names unconstrained", () => {
    const cs = [...installed, b("other", "9.0.0")];
    expect(pick(cs, [{ name: "studio", version: "0.1.23" }])).toBe("studio@0.1.23");
    // No studio bundle qualifies, so a bundle no loaded extension names is used.
    expect(pick(cs, [{ name: "studio", version: "0.1.0" }])).toBe("other@9.0.0");
    // A loaded extension that ships no bundle holds nothing back.
    expect(pick(cs, [{ name: "probe", version: "0.0.1" }])).toBe("other@9.0.0");
  });

  it("does not reorder its input", () => {
    const cs = [...installed];
    selectBundle(cs, []);
    expect(versions(cs)).toEqual(versions(installed));
  });
});

describe("UiBundleStore with an extension loaded", () => {
  it("serves the bundle at the extension's version and follows a change of extension", () => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "ue-mcp-ui-select-"));
    try {
      const keys = crypto.generateKeyPairSync("ed25519");
      const pem = keys.privateKey.export({ type: "pkcs8", format: "pem" }).toString();
      const store = new UiBundleStore(DAEMON_API_VERSION, path.join(root, "ui"));
      store.trustKey(keys.publicKey.export({ type: "spki", format: "pem" }).toString());
      for (const v of ["1.0.0", "1.1.0", "2.0.0"]) {
        const dir = path.join(root, "build", v);
        fs.mkdirSync(dir, { recursive: true });
        fs.writeFileSync(path.join(dir, "index.html"), v);
        signBundle(dir, { name: "studio", version: v, api: { min: DAEMON_API_VERSION, max: DAEMON_API_VERSION } }, pem);
        store.install(dir);
      }
      expect(store.active()?.manifest.version).toBe("2.0.0");
      store.setOwners([{ name: "studio", version: "1.1.0" }]);
      expect(store.active()?.manifest.version).toBe("1.1.0");
      expect(fs.readFileSync(store.resolve("/ui/")!, "utf-8")).toBe("1.1.0");
      store.setOwners([{ name: "studio", version: "1.5.0" }]);
      expect(store.active()?.manifest.version).toBe("1.1.0");
      store.setOwners([]);
      expect(store.active()?.manifest.version).toBe("2.0.0");
    } finally {
      fs.rmSync(root, { recursive: true, force: true });
    }
  });
});
