// Duplicate file-local definition audit.
//
// No shebang: this file is run by `node` from npm scripts and re-imported by
// vitest, whose loader chokes on the shebang line as "Invalid or unexpected
// token" and takes the whole test file down with it. Invoke as
// `node scripts/audit-unity-collisions.mjs` (already the `audit:unity` shape).
//
// UBT compiles a module as a unity build: several .cpp files are concatenated
// into one translation unit. An anonymous namespace is per translation unit, so
// two files can each define a file-local helper of the same name and both
// compile in isolation - right up until the grouping puts them in the same
// blob, and the two anonymous namespaces merge into one. Then the second
// definition is a redefinition: error C2084, "function already has a body".
//
// The grouping is not stable. It shifts with file count, file order, and the
// adaptive-unity working set, which UBT derives from `git status`. That is why
// a duplicate can sit in a release for weeks, build clean on the machine that
// wrote it, and break on a user's first build of the same source.
//
// This audit reads what the compiler would see and reports any file-local
// function defined with the same signature in two files of one module.
// Overloads are fine (that is what C++ does with them), so the signature, not
// just the name, is the key.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(__dirname, '..');

/** Every `namespace {` block opened at file scope, returned as its inner text. */
function anonymousNamespaceBodies(source) {
  const bodies = [];
  const opener = /^namespace\s*(?:\/\/[^\n]*\n|\/\*[\s\S]*?\*\/|\s)*\{/gm;
  let match;
  while ((match = opener.exec(source)) !== null) {
    const open = source.indexOf('{', match.index);
    let depth = 0;
    let i = open;
    for (; i < source.length; i++) {
      if (source[i] === '{') depth++;
      else if (source[i] === '}' && --depth === 0) break;
    }
    if (depth === 0) bodies.push(source.slice(open + 1, i));
  }
  return bodies;
}

/** Strip comments and string/char literals so braces inside them cannot fool the scan. */
function stripNonCode(source) {
  return source
    .replace(/\\["']/g, '__')
    .replace(/"(?:[^"\\\n]|\\.)*"/g, '""')
    .replace(/'(?:[^'\\\n]|\\.)*'/g, "''")
    .replace(/\/\*[\s\S]*?\*\//g, ' ')
    .replace(/\/\/[^\n]*/g, ' ');
}

/** Split a parameter list on top-level commas, ignoring those inside <> ( ) [ ]. */
function splitParams(list) {
  const out = [];
  let depth = 0;
  let buf = '';
  for (const ch of list) {
    if (ch === '<' || ch === '(' || ch === '[') depth++;
    else if (ch === '>' || ch === ')' || ch === ']') depth--;
    if (ch === ',' && depth === 0) {
      out.push(buf);
      buf = '';
      continue;
    }
    buf += ch;
  }
  if (buf.trim()) out.push(buf);
  return out;
}

/** "const FString& Path = X" -> "const FString&". The parameter name never
 *  participates in overload resolution, so it must not participate here. */
function normalizeParamType(param) {
  let p = param.split('=')[0];
  p = p.replace(/([*&])/g, ' $1 ');
  const tokens = p.split(/\s+/).filter(Boolean);
  if (tokens.length > 1 && /^[A-Za-z_]\w*$/.test(tokens[tokens.length - 1])) {
    const prior = tokens[tokens.length - 2];
    // Drop the trailing token only when it is a name, not the type itself.
    if (!['const', 'volatile', 'struct', 'class', 'unsigned', 'signed', 'long', 'short'].includes(prior)) {
      tokens.pop();
    }
  }
  return tokens.join(' ');
}

/** File-local function definitions at the top level of an anonymous namespace. */
function fileLocalFunctions(body) {
  const found = [];
  let depth = 0;
  let buf = '';
  for (let i = 0; i < body.length; i++) {
    const ch = body[i];
    if (ch === '{') {
      if (depth === 0) {
        const decl = buf.replace(/\s+/g, ' ').trim();
        // A definition, not a call: `Name(params)` optionally trailed by
        // const/noexcept/override, and not a control-flow keyword.
        const m = decl.match(/(?:^|[\s*&>])([A-Za-z_]\w*)\s*\(([^()]*)\)\s*(?:const\s*)?(?:noexcept\s*)?$/);
        if (m && !['if', 'for', 'while', 'switch', 'catch', 'return', 'else', 'do'].includes(m[1])) {
          // A leading return type is what separates a definition from a
          // constructor-style expression at namespace scope.
          const head = decl.slice(0, decl.lastIndexOf(m[1])).trim();
          if (head.length > 0) {
            const params = splitParams(m[2]).map(normalizeParamType).filter((p) => p && p !== 'void');
            found.push({ name: m[1], signature: `${m[1]}(${params.join(', ')})` });
          }
        }
        buf = '';
      }
      depth++;
      continue;
    }
    if (ch === '}') {
      depth--;
      continue;
    }
    if (depth === 0) {
      if (ch === ';') buf = '';
      else buf += ch;
    }
  }
  return found;
}

/** Modules are the unity boundary: `.../Source/<Module>/...`. */
function moduleOf(file) {
  const parts = file.split(/[\\/]/);
  const idx = parts.lastIndexOf('Source');
  return idx >= 0 && parts[idx + 1] ? parts[idx + 1] : path.dirname(file);
}

function collectSources(root) {
  const out = [];
  if (!fs.existsSync(root)) return out;
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const full = path.join(root, entry.name);
    if (entry.isDirectory()) out.push(...collectSources(full));
    else if (entry.isFile() && entry.name.endsWith('.cpp')) out.push(full);
  }
  return out;
}

export function findUnityCollisions(root = path.join(repoRoot, 'plugin')) {
  const byModule = new Map();
  for (const file of collectSources(root)) {
    const source = stripNonCode(fs.readFileSync(file, 'utf8'));
    const mod = moduleOf(file);
    if (!byModule.has(mod)) byModule.set(mod, new Map());
    const signatures = byModule.get(mod);
    for (const body of anonymousNamespaceBodies(source)) {
      for (const fn of fileLocalFunctions(body)) {
        if (!signatures.has(fn.signature)) signatures.set(fn.signature, new Set());
        signatures.get(fn.signature).add(path.relative(repoRoot, file));
      }
    }
  }

  const collisions = [];
  for (const [mod, signatures] of byModule) {
    for (const [signature, files] of signatures) {
      if (files.size > 1) {
        collisions.push({ module: mod, signature, files: [...files].sort() });
      }
    }
  }
  return collisions.sort((a, b) => a.signature.localeCompare(b.signature));
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const collisions = findUnityCollisions();
  if (collisions.length === 0) {
    console.log('audit:unity-collisions - no duplicate file-local definitions');
    process.exit(0);
  }
  console.error(`audit:unity-collisions - ${collisions.length} duplicate file-local definition(s):\n`);
  for (const c of collisions) {
    console.error(`  [${c.module}] ${c.signature}`);
    for (const f of c.files) console.error(`      ${f}`);
    console.error('');
  }
  console.error('Each of these compiles alone and fails when unity puts the files in one blob.');
  console.error('Hoist the helper into a shared header, or give the copies distinct signatures.');
  process.exit(1);
}
