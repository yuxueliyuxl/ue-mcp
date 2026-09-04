import { z } from "zod";
import { categoryTool, bp, type ToolDef } from "../types.js";
import { Vec3, Rotator } from "../schemas.js";
import { SESSION_ID } from "../locking.js";
import { McpError, ErrorCode } from "../errors.js";
import type { EditorSession } from "../session.js";
import type { ToolContext } from "../types.js";

/**
 * Who a lock belongs to: the addressed editor, or this process when there is
 * no session behind the call (#817).
 *
 * The lock registry lives in the bridge, which is per editor, so the owner has
 * to match whatever `withAssetLocks` used on the dispatch path or an explicit
 * asset(unlock) would not match the lock asset(lock) took. Both read this.
 */
function lockOwner(ctx: ToolContext, params: Record<string, unknown>): string {
  const explicit = params.sessionId;
  if (typeof explicit === "string" && explicit.trim() !== "") return explicit;
  return ctx.session?.lockOwnerId ?? SESSION_ID;
}

/**
 * `asset(migrate)`, with a second editor as the destination (#817, plan 6.5).
 *
 * Migration is the one action with two editors in it: the call runs in the
 * editor holding the source assets and its output lands in another project
 * entirely. Naming that project as a path worked, but left two things to the
 * caller that the server already knows: which directory it is, and the fact
 * that the destination editor will not see the new packages until its asset
 * registry is rescanned. A destination editor answers both.
 *
 * The migrate call itself is unchanged and still goes to the source editor's
 * bridge; `toEditor` never reaches it.
 */
async function migrateAssets(
  ctx: ToolContext,
  p: Record<string, unknown>,
): Promise<unknown> {
  const requested = typeof p.toEditor === "string" ? p.toEditor.trim() : "";
  const explicitDir = typeof p.destinationContentDir === "string" ? p.destinationContentDir.trim() : "";

  let destination: EditorSession | undefined;
  let destinationContentDir = explicitDir;

  if (requested) {
    if (explicitDir) {
      throw new McpError(
        ErrorCode.INVALID_PARAMS,
        "Pass 'toEditor' or 'destinationContentDir', not both: they name the same thing and " +
          "there is no safe answer when they disagree.",
      );
    }
    if (!ctx.sessions) {
      throw new McpError(
        ErrorCode.INVALID_PARAMS,
        "'toEditor' addresses another editor this server drives, and there is no session registry here. " +
          "Pass 'destinationContentDir' with the target project's Content folder instead.",
      );
    }
    destination = ctx.sessions.resolve(requested);
    if (ctx.session && destination === ctx.session) {
      throw new McpError(
        ErrorCode.INVALID_PARAMS,
        `'${destination.name}' is the editor this call runs in, so there is nothing to migrate between. ` +
          "Address the source editor with 'editor' and the destination with 'toEditor'.",
      );
    }
    const dir = destination.project.contentDir;
    if (!dir) {
      throw new McpError(
        ErrorCode.INVALID_PARAMS,
        `Editor '${destination.name}' has no project bound, so it has no Content directory to migrate into.`,
      );
    }
    destinationContentDir = dir;
  }

  const result = (await ctx.bridge.call("migrate", {
    assetPaths: p.assetPaths,
    assetPath: p.assetPath,
    destinationContentDir,
    includeDependencies: p.includeDependencies,
    onConflict: p.onConflict,
    allowDirty: p.allowDirty,
    dryRun: p.dryRun,
  })) as Record<string, unknown>;

  if (!destination) return result;

  const out: Record<string, unknown> = {
    ...(result && typeof result === "object" ? result : { result }),
    destination: {
      editor: destination.name,
      project: destination.project.projectPath,
      contentDir: destinationContentDir,
    },
  };
  out.rescan = p.dryRun === true
    ? { attempted: false, reason: "dryRun copied nothing, so there is nothing to rescan." }
    : await rescanDestination(destination, contentPathsOf(p));
  return out;
}

/** The content directories a migrate landed in, derived from what it was asked to move. */
function contentPathsOf(p: Record<string, unknown>): string[] {
  const raw = [
    ...(Array.isArray(p.assetPaths) ? p.assetPaths : []),
    ...(typeof p.assetPath === "string" ? [p.assetPath] : []),
  ].filter((v): v is string => typeof v === "string" && v.startsWith("/"));

  const dirs = new Set<string>();
  for (const assetPath of raw) {
    const withoutObject = assetPath.split(".")[0];
    const slash = withoutObject.lastIndexOf("/");
    dirs.add(slash > 0 ? withoutObject.slice(0, slash) : withoutObject);
  }
  // Nothing recognisable to narrow by: rescan the whole game root rather than
  // leave the destination editor blind to what just landed in it.
  if (dirs.size === 0) return ["/Game"];
  return [...dirs].slice(0, 32);
}

/**
 * Make the migrated packages visible in the destination editor.
 *
 * Unreal does not notice files that appeared under Content while it was
 * running, so without this the assets are on disk and absent from every
 * registry query until someone restarts or rescans by hand. `diagnose_registry`
 * with `reconcile` is a forced synchronous scan of one path, which is exactly
 * the operation needed and already exists on the bridge.
 *
 * Best-effort by design: the migration itself has already succeeded, so a
 * destination editor that is closed or on an older plugin is reported, not
 * turned into a failure of a copy that worked.
 */
async function rescanDestination(
  destination: EditorSession,
  contentPaths: string[],
): Promise<Record<string, unknown>> {
  if (!destination.bridge.isConnected) {
    return {
      attempted: false,
      reason:
        `Editor '${destination.name}' is not connected, so its asset registry could not be rescanned. ` +
        "The files are on disk; that editor will see them when it next starts.",
    };
  }
  const scanned: string[] = [];
  const failed: Array<{ path: string; error: string }> = [];
  for (const contentPath of contentPaths) {
    try {
      await destination.guarded.call("diagnose_registry", {
        path: contentPath,
        recursive: true,
        reconcile: true,
      });
      scanned.push(contentPath);
    } catch (e) {
      failed.push({ path: contentPath, error: e instanceof Error ? e.message : String(e) });
    }
  }
  return failed.length === 0
    ? { attempted: true, editor: destination.name, scanned }
    : { attempted: true, editor: destination.name, scanned, failed };
}

export const assetTool: ToolDef = categoryTool(
  "asset",
  "Asset management: list, search, read, CRUD, import meshes/textures, datatables, stringtables.",
  {
    list: bp(
      "List assets via the AssetRegistry (sees /Game and every mounted plugin root). Paginated: returns totalMatched, offset, hasMore and nextOffset so a large folder can be walked deterministically instead of dropping the bridge on one oversized response (#790). Params: directory? (default /Game), classFilter?, recursive? (default true), maxResults? (default 500, max 5000), offset? (default 0)",
      "list_assets",
      (p) => ({ directory: p.directory, classFilter: p.classFilter ?? p.typeFilter, recursive: p.recursive, maxResults: p.maxResults, offset: p.offset }),
    ),
    search: {
      description: "Search by name/class/path. Params: query, directory?, maxResults?, searchAll?",
      handler: async (ctx, p) => {
        const { action: _, ...rest } = p;
        const roots = ctx.project.config.contentRoots;
        // If no directory specified and contentRoots configured, search each root and merge
        if (!p.directory && roots && roots.length > 0) {
          const maxResults = (p.maxResults as number) ?? 50;
          const allResults: Array<Record<string, unknown>> = [];
          for (const root of roots) {
            const res = await ctx.bridge.call("search_assets", { ...rest, directory: root }) as Record<string, unknown>;
            if (res.results && Array.isArray(res.results)) {
              allResults.push(...(res.results as Array<Record<string, unknown>>));
            }
            if (allResults.length >= maxResults) break;
          }
          return {
            query: p.query ?? "",
            searchScope: roots,
            resultCount: Math.min(allResults.length, maxResults),
            results: allResults.slice(0, maxResults),
            success: true,
          };
        }
        return ctx.bridge.call("search_assets", rest);
      },
    },
    read:           bp("Read asset via reflection. Params: assetPath", "read_asset", (p) => ({ path: p.assetPath })),
    read_properties: bp("Read asset properties with values. Blueprint paths resolve to the generated-class CDO (#568). propertyName accepts dotted/indexed paths into nested structs, array elements, and instanced subobjects (e.g. `Config.Traits[1].Params.Field`); landing on an array of subobjects also lists each element's index+class (#527). expandDepth inlines the properties of subobjects OWNED by this asset, so a data asset's nested payload comes back in ONE call instead of a reference you have to chase (#755); references to OTHER assets are marked expandable rather than followed, unless expandExternal=true. Capped by maxExpandedObjects with expansionTruncated reported. Params: assetPath, propertyName?, includeValues?, valueFormat?, expandDepth? (0-5, default 0), expandExternal?, maxExpandedObjects? (default 64)", "read_asset_properties", (p) => ({ assetPath: p.assetPath, propertyName: p.propertyName, includeValues: p.includeValues, valueFormat: p.valueFormat, expandDepth: p.expandDepth, expandExternal: p.expandExternal, maxExpandedObjects: p.maxExpandedObjects })),
    list_properties: bp("List reflected properties on any asset. Params: assetPath, includeValues?, valueFormat? ('text'|'json')", "read_asset_properties", (p) => ({ assetPath: p.assetPath ?? p.path, includeValues: p.includeValues, valueFormat: p.valueFormat })),
    get_properties: bp("Read property values on any asset. propertyName accepts dotted/indexed paths into nested structs, array elements, and instanced subobjects. valueFormat='json' returns structured values. Params: assetPath, propertyName?, includeValues?, valueFormat?", "read_asset_properties", (p) => ({ assetPath: p.assetPath ?? p.path, propertyName: p.propertyName, includeValues: p.includeValues ?? true, valueFormat: p.valueFormat })),
    duplicate:      bp("Duplicate asset. Params: sourcePath, destinationPath", "duplicate_asset"),
    rename:         bp("Rename asset. Params: assetPath, newName (or sourcePath, destinationPath), force?. World Partition levels are detected and their __ExternalActors__/__ExternalObjects__ packages migrate atomically alongside the .umap, source-side redirectors get fixed up, and the active editor world is swapped to blank if it matches the source (#409). Refuses if any package is dirty - save first. If a prior rename left externals orphaned at the old path, re-running reconciles them. Rollback descriptor is emitted even on partial failure so the inverse rename can recover. `force=true` lets the call merge into a destination with pre-existing externals (used by rollback). For batches of 3+ scene-referenced non-world assets use bulk_rename instead.", "rename_asset"),
    bulk_rename:    bp("Batched rename using IAssetTools::RenameAssets - single transaction with one redirector-fixup pass (matches Content Browser drag). Use this over looped rename for scene-referenced assets. World assets are rejected (status=rejected_world); use rename_asset which handles WP externals atomically (#409). The fix-up pass cannot see a referencer that is not loaded, so each renamed item reports redirectorLeft and the result carries redirectorsLeft/redirectorsRemoved plus redirectorPackages, which feeds straight into fixup_redirectors (#908). Params: renames[] where each entry is {sourcePath, destinationPath} OR {assetPath, newName}.", "bulk_rename_assets", (p) => ({ renames: p.renames })),
    fixup_redirectors: bp("Clean up ObjectRedirectors left at the old paths after a move, bounded to exactly the packages you name. Preflights each redirector, its destination, and its hard AND soft referencers, then reports packagesToLoad/packagesToSave before touching anything; dryRun=true stops there. The real run loads only those referencers (a soft reference in an unloaded package is what the rename's own fix-up pass misses), rewrites them, saves them, RE-QUERIES the asset registry, and deletes a redirector only when nothing references it any more, reporting kept vs deleted per package with the referencers that survived. Naming a whole content root is refused unless allowProjectWide=true, so this can never turn into a project-wide resave. Protected mounts are refused outright. dryRun is the way to preview: save=false only skips the explicit save pass, because the editor's own fix-up writes what it can regardless. Params: paths[] (redirector packages or the folders holding them), dryRun? (default false), save? (default true), allowProjectWide? (default false) (#908)", "fixup_redirectors", (p) => ({ paths: p.paths, dryRun: p.dryRun, save: p.save, allowProjectWide: p.allowProjectWide })),
    move:           bp("Move asset. Params: sourcePath, destinationPath", "move_asset"),
    delete:         bp("Delete asset. force defaults to false and is a REAL guard: the bridge asks the Asset Registry for referencers first and refuses with success=false, reason='has_referencers' and the full referencers list rather than destroying an asset other packages point at (#976). force=true takes the force-delete path, which also auto-closes open asset editors (#278). On a delete the editor attempted and could not finish, the reason is open_in_editor / has_referencers / in_memory_referenced / package_read_only / package_dirty / unknown, with referencers, inMemoryReferencers, packageReadOnly and packageDirty diagnostics (#601). Params: assetPath, force?", "delete_asset"),
    delete_batch:   bp("Batch-delete assets. Per-path status is deleted | absent | protected | refused | failed, plus reason+referencers on refused and failed entries. A referenced asset is refused per entry with force=false and the rest of the batch still runs, so one guarded asset never hides behind a bare failed count (#976, #278). Result carries deleted/absent/refused/failed/protected/total. Params: assetPaths[], force?", "delete_asset_batch"),
    create_data_asset: bp("Create UDataAsset instance of custom class. className accepts the C++ spelling with or without the A/U/F/E prefix (UMyConfig and MyConfig both resolve), a /Script/Module.ClassName path, or a loaded class name; a failed lookup lists the spellings tried and the closest matches (#823). Params: name, className, packagePath?, properties? (key/value map)", "create_data_asset"),
    create_asset_by_class: bp("Create an asset of ANY concrete UObject class (not just UDataAsset) - physical-material subclasses, curves, settings objects. className accepts the C++ spelling with or without the A/U/F/E prefix, a /Script/Module.ClassName path, or a loaded class name (#823). Params: name, className, packagePath?, properties? (key/value map), onConflict? (skip|replace|rename). Actors/components and specialized assets (Blueprint/Material) have dedicated actions (#726)", "create_asset_by_class", (p) => ({ name: p.name, className: p.className, packagePath: p.packagePath, properties: p.properties, onConflict: p.onConflict })),
    create_subobject: bp("Create a named subobject OWNED by an existing asset, inside that asset's package, and return its object path for later property writes. This is the missing half of editing a data asset whose payload is named subobjects referenced from a struct array: set_property, bulk_set_properties and append_array_elements can edit the entries, and this makes a new one. Component classes are accepted, unlike create_asset_by_class; Actors are refused because they are spawned into a level. className takes a /Script/Module.Class path, which is how you name a plugin class the Python `unreal` module never exposes. The new object is created RF_Standalone and the package is saved in the same call, so it survives the garbage collection that used to eat a fresh object within one call and is reachable by path from the next one. properties are applied to a throwaway instance first, so a bad path or value creates nothing. outer='asset' (default) gives '<asset>.<name>'; outer='package' puts it beside the asset in the package. Params: assetPath, className, name, properties?, outer? (asset|package), onConflict? (reuse|error), save? (default true) (#975)", "create_subobject", (p) => ({ assetPath: p.assetPath ?? p.path, className: p.className, name: p.name, properties: p.properties, outer: p.outer, onConflict: p.onConflict, save: p.save })),
    bulk_upsert_data_assets: bp("Create or update up to 500 UDataAsset instances in ONE call. Every descriptor is first applied to a transient copy, so a bad class, property path, or value rejects the whole batch before a package is touched; nothing is half-written by a typo. Per-item status is created | updated | unchanged | skipped | failed (dryRun reports wouldCreate | wouldUpdate | wouldRemainUnchanged | wouldSkip), each with its own error when it failed. Replaying the same request returns unchanged, and only changed packages are saved. Emits a rollback descriptor that restores prior values and deletes what it created. Params: items[]: [{name, packagePath, className, properties?}], onConflict? (update (default) | skip | error), dryRun? (default false), save? (default true)", "bulk_upsert_data_assets", (p) => ({ items: p.items, onConflict: p.onConflict, dryRun: p.dryRun, save: p.save })),
    save:           bp("Save one asset, or every dirty asset under /Game when assetPath is omitted. force=true saves regardless of the dirty flag - several edits (OFPA level actors, some subsystem property writes) never mark their package dirty, so a dirty-only save skipped them and still reported success. Returns the package name plus on-disk file path, size and mtime so the write can be verified rather than trusted (#768). Params: assetPath?, force?", "save_asset", (p) => ({ assetPath: p.assetPath ?? p.path, force: p.force })),
    save_all_dirty: bp("Flush every dirty package to disk in one call. Reports the packages it attempted, which ones reached disk (with file path, size and mtime) and which are still dirty afterwards, because a bare savedAll boolean has come back true while packages were never written (#768). Params: saveMapPackages? (default true), saveContentPackages? (default true)", "save_all_dirty", (p) => ({ saveMapPackages: p.saveMapPackages, saveContentPackages: p.saveContentPackages })),
    set_mesh_material:    bp("Assign material to static mesh slot. Params: assetPath, materialPath, slotIndex?", "set_mesh_material"),
    set_mesh_materials_batch: bp("Assign materials across many meshes and slots in one call, so an N mesh x M slot kit costs one round trip instead of N*M. StaticMesh and SkeletalMesh both work. Address a slot by slotName (survives a reimport reordering slot indices) or by slotIndex (default 0); passing both is rejected when they disagree. Every submitted assignment returns its own index/ok/status/error, status being ok|updated|unchanged|invalid|protected|duplicate|not_found|slot_not_found|failed|skipped. Default is all-or-nothing: any preflight rejection aborts before a mesh is touched. Pass continueOnError to apply the assignments that did pass and keep the rejects reported alongside them. Each mesh is written and saved once no matter how many of its slots the batch names, and the rollback payload restores only the writes that landed. Params: assignments ([{assetPath, materialPath, slotName? | slotIndex?}], max 500), save? (default true), dryRun? (default false), continueOnError? (default false) (#822)", "set_mesh_materials_batch", (p) => ({ assignments: p.assignments, save: p.save, dryRun: p.dryRun, continueOnError: p.continueOnError })),
    recenter_pivot:       { description: "Move static mesh pivot to geometry center. Params: assetPath OR assetPaths", bridge: "recenter_pivot", mapParams: (p) => {
      const paths = p.assetPaths as string[] | undefined;
      if (paths && paths.length > 0) return { assetPaths: paths };
      return { assetPath: p.assetPath };
    }},
    import_static_mesh:   bp("Import from FBX, OBJ, or GLB/glTF (glTF routes through Interchange) (#549). importUniformScale=100 fixes metre-authored FBX (#687). Params: filePath, name?, packagePath?, combineMeshes?, importMaterials?, importTextures?, generateLightmapUVs?, importUniformScale?", "import_static_mesh", (p) => ({ filename: p.filePath, destinationPath: p.packagePath, assetName: p.name, combineMeshes: p.combineMeshes, importMaterials: p.importMaterials, importTextures: p.importTextures, generateLightmapUVs: p.generateLightmapUVs, importUniformScale: p.importUniformScale })),
    import_skeletal_mesh: bp("Import skeletal mesh from FBX. importUniformScale=100 fixes metre-authored FBX (Blender FBX_SCALE_ALL) that lands 100x too small on a cm skeleton (#687). Returns post-import readback: boxExtent, morphTargets[], numLODs, skeleton (#678). Params: filePath, name?, packagePath?, skeletonPath?, importMaterials?, importTextures?, importUniformScale? (default 1.0), importMorphTargets? (default true), createPhysicsAsset? (default false), replaceExisting? (default true)", "import_skeletal_mesh", (p) => ({ filename: p.filePath, destinationPath: p.packagePath, assetName: p.name, skeletonPath: p.skeletonPath, importMaterials: p.importMaterials, importTextures: p.importTextures, importUniformScale: p.importUniformScale, importMorphTargets: p.importMorphTargets, createPhysicsAsset: p.createPhysicsAsset, replaceExisting: p.replaceExisting })),
    import_animation:     bp("Import anim from FBX. Params: filePath, name?, packagePath?, skeletonPath", "import_animation", (p) => ({ filename: p.filePath, destinationPath: p.packagePath, assetName: p.name, skeletonPath: p.skeletonPath })),
    import_texture:       bp("Import image. sRGB/compressionSettings/lodGroup/neverStream are applied at import time (folded in, no second call needed) (#661). Params: filePath, name?, packagePath?, sRGB?, compressionSettings? (Default|Normalmap|Grayscale|HDR|BC7|...), lodGroup?, neverStream?", "import_texture", (p) => ({ filename: p.filePath, destinationPath: p.packagePath, assetName: p.name, sRGB: p.sRGB, compressionSettings: p.compressionSettings, lodGroup: p.lodGroup, neverStream: p.neverStream })),
    create_render_target_2d: bp("Create and persist a TextureRenderTarget2D asset. Render format is applied before resource initialization. Params: name, packagePath? (default /Game), width? (1-8192, default 512), height? (1-8192, default 512), format? (R8|RG8|RGBA8|RGBA8_SRGB|R16F|RG16F|RGBA16F|R32F|RG32F|RGBA32F|RGB10A2, default RGBA8_SRGB), clearColor? ({r,g,b,a}, default transparent), generateMips? (default false), targetGamma? (default 0), onConflict? (skip|error)", "create_render_target_2d", (p) => ({ name: p.name, packagePath: p.packagePath, width: p.width, height: p.height, format: p.format, clearColor: p.clearColor, generateMips: p.generateMips, targetGamma: p.targetGamma, onConflict: p.onConflict })),
    read_cloth_data:      bp("Read Chaos cloth data on a skeletal mesh: per clothing asset, its configs (reflected properties), LOD count, and per-LOD point-weight-map summary (name, target, vertex count, min/max - including the MaxDistances mask). Params: skeletalMeshPath (#595)", "read_cloth_data", (p) => ({ skeletalMeshPath: p.skeletalMeshPath })),
    set_cloth_config:     bp("Set properties on a clothing asset's Chaos cloth config via reflection. Params: skeletalMeshPath, properties (object), clothingAsset? (name filter), configType? (config class/key filter) (#595)", "set_cloth_config", (p) => ({ skeletalMeshPath: p.skeletalMeshPath, properties: p.properties, clothingAsset: p.clothingAsset, configType: p.configType })),
    export_texture:       bp("Export a Texture2D to a PNG on disk (for inspection or external diffing). Params: assetPath, outputPath (.png) (#697)", "export_texture", (p) => ({ assetPath: p.assetPath, outputPath: p.outputPath })),
    compare_textures:     bp("Compare two Texture2D assets by dimensions, pixel format, and source-content identity (FTextureSource id) - tells you whether an authored texture actually changed without offline pixel-diffing. Params: assetPathA, assetPathB (#697)", "compare_textures", (p) => ({ assetPathA: p.assetPathA, assetPathB: p.assetPathB })),
    import_texture_batch: bp("Import many textures in one call - the loop stays inside the editor (no per-file bridge round-trip), so this finishes far faster than N import_texture calls. Per-item result records mirror import_texture. Params: items[]: [{filePath, packagePath?, name?, replaceExisting?}], packagePath? (default for items that don't set it), save? (default true), automated? (default true). Returns requested/imported/failed counts + items[] (#430)", "import_texture_batch", (p) => ({ items: p.items, packagePath: p.packagePath, save: p.save, automated: p.automated })),
    reimport:             bp("Reimport asset from source file. Params: assetPath, filePath?", "reimport_asset", (p) => ({ assetPath: p.assetPath, filePath: p.filePath })),
    read_datatable:       bp("Read DataTable rows. Params: assetPath, rowFilter?", "read_datatable", (p) => ({ path: p.assetPath, rowFilter: p.rowFilter })),
    create_datatable:     bp("Create DataTable. Params: name, packagePath?, rowStruct", "create_datatable"),
    reimport_datatable:   bp("Reimport DataTable from JSON. Params: assetPath, jsonPath?, jsonString?", "reimport_datatable", (p) => ({ path: p.assetPath, jsonPath: p.jsonPath, jsonString: p.jsonString })),
    set_datatable_row:    bp("Append or overwrite a single DataTable row. Params: assetPath, rowName, row (object with row-struct fields - partial updates merge with the existing row). Idempotent; rollback restores the prior row (#437)", "set_datatable_row", (p) => ({ assetPath: p.assetPath, rowName: p.rowName, row: p.row ?? p.fields ?? p.data })),
    add_datatable_row:    bp("Alias for set_datatable_row (#437)", "add_datatable_row", (p) => ({ assetPath: p.assetPath, rowName: p.rowName, row: p.row ?? p.fields ?? p.data })),
    update_datatable_row: bp("Alias for set_datatable_row; partial update merges with existing row (#437)", "update_datatable_row", (p) => ({ assetPath: p.assetPath, rowName: p.rowName, row: p.row ?? p.fields ?? p.data })),
    remove_datatable_row: bp("Remove a single DataTable row. Idempotent (alreadyDeleted=true if missing). Params: assetPath, rowName (#437)", "remove_datatable_row", (p) => ({ assetPath: p.assetPath, rowName: p.rowName })),
    get_datatable_row:    bp("Read one DataTable row's fields without dumping the whole table. Params: assetPath, rowName (#535)", "get_datatable_row", (p) => ({ assetPath: p.assetPath, rowName: p.rowName })),
    set_datatable_cell:   bp("Write a single field on a single existing row (merges, leaves other cells untouched). Errors if the row doesn't exist. Params: assetPath, rowName, fieldName, value (#535)", "set_datatable_cell", (p) => ({ assetPath: p.assetPath, rowName: p.rowName, fieldName: p.fieldName, value: p.value })),
    rename_datatable_row: bp("Rename a row key, preserving its values. Params: assetPath, oldName, newName (#535)", "rename_datatable_row", (p) => ({ assetPath: p.assetPath, oldName: p.oldName ?? p.rowName, newName: p.newName })),
    fill_datatable_from_json: bp("Bulk-upsert rows from a {rowName: {field: value}} object without touching unrelated rows (non-destructive, unlike reimport_datatable). Params: assetPath, rows (object) or jsonString (#535)", "fill_datatable_from_json", (p) => ({ assetPath: p.assetPath, rows: p.rows, jsonString: p.jsonString })),
    create_curvetable:    bp("Create CurveTable asset. Params: name, packagePath?, onConflict?", "create_curvetable"),
    read_curvetable:      bp("Read CurveTable rows and keys. Params: assetPath, rowFilter?", "read_curvetable", (p) => ({ assetPath: p.assetPath, rowFilter: p.rowFilter })),
    list_curvetable_rows: bp("Alias for read_curvetable. Params: assetPath, rowFilter?", "list_curvetable_rows", (p) => ({ assetPath: p.assetPath, rowFilter: p.rowFilter })),
    import_curvetable:    bp("Import CurveTable from JSON/CSV string or file. Params: assetPath, jsonString?, csvString?, filePath?, format?, interpMode?", "import_curvetable", (p) => ({ assetPath: p.assetPath, jsonString: p.jsonString, csvString: p.csvString, filePath: p.filePath, format: p.format, interpMode: p.interpMode })),
    add_curvetable_row:   bp("Add CurveTable row. Params: assetPath, rowName, curveType? ('simple'|'rich'), interpMode?", "add_curvetable_row", (p) => ({ assetPath: p.assetPath, rowName: p.rowName, curveType: p.curveType, mode: p.mode, interpMode: p.interpMode })),
    remove_curvetable_row: bp("Remove CurveTable row. Idempotent if missing. Params: assetPath, rowName", "remove_curvetable_row", (p) => ({ assetPath: p.assetPath, rowName: p.rowName })),
    rename_curvetable_row: bp("Rename CurveTable row. Params: assetPath, oldName, newName", "rename_curvetable_row", (p) => ({ assetPath: p.assetPath, oldName: p.oldName ?? p.rowName, newName: p.newName })),
    get_curvetable_keys:  bp("Read keys from one CurveTable row. Params: assetPath, rowName", "get_curvetable_keys", (p) => ({ assetPath: p.assetPath, rowName: p.rowName })),
    set_curvetable_keys:  bp("Replace keys on one CurveTable row. Params: assetPath, rowName, keys:[{time,value,interpMode?,arriveTangent?,leaveTangent?}]", "set_curvetable_keys", (p) => ({ assetPath: p.assetPath, rowName: p.rowName, keys: p.keys })),
    add_curvetable_key:   bp("Add or update one key on a CurveTable row. Params: assetPath, rowName, time, value, interpMode?, keyTimeTolerance?", "add_curvetable_key", (p) => ({ assetPath: p.assetPath, rowName: p.rowName, time: p.time, value: p.value, interpMode: p.interpMode, keyTimeTolerance: p.keyTimeTolerance })),
    list_textures:        bp("List textures. Params: directory?, recursive?", "list_textures"),
    get_texture_info:     bp("Get texture details. Params: assetPath", "get_texture_info"),
    set_texture_settings: bp("Set texture settings. Params: assetPath, settings (object with compressionSettings?, lodGroup?, sRGB?, neverStream?). Keys may also be passed at the top level.", "set_texture_settings", (p) => ({
      assetPath: p.assetPath,
      ...(typeof p.settings === "object" && p.settings !== null ? p.settings : {}),
      ...(p.compressionSettings !== undefined ? { compressionSettings: p.compressionSettings } : {}),
      ...(p.lodGroup !== undefined ? { lodGroup: p.lodGroup } : {}),
      ...(p.sRGB !== undefined ? { sRGB: p.sRGB } : {}),
      ...(p.neverStream !== undefined ? { neverStream: p.neverStream } : {}),
    })),
    create_stringtable:   bp("Create a StringTable asset. Params: name, packagePath?, namespace?, onConflict?", "create_stringtable"),
    read_stringtable:     bp("Read StringTable entries and keys. Params: assetPath, keyFilter?", "read_stringtable", (p) => ({ assetPath: p.assetPath, path: p.path, keyFilter: p.keyFilter })),
    list_stringtable_keys: bp("List StringTable keys. Params: assetPath, keyFilter?", "list_stringtable_keys", (p) => ({ assetPath: p.assetPath, path: p.path, keyFilter: p.keyFilter })),
    get_stringtable_entry: bp("Read one StringTable entry. Params: assetPath, key", "get_stringtable_entry", (p) => ({ assetPath: p.assetPath, path: p.path, key: p.key })),
    set_stringtable_entry: bp("Create or update one StringTable entry. Params: assetPath, key, sourceString (or value)", "set_stringtable_entry", (p) => ({ assetPath: p.assetPath, path: p.path, key: p.key, sourceString: p.sourceString, value: p.value })),
    remove_stringtable_entry: bp("Remove one StringTable entry. Idempotent (alreadyDeleted=true if missing). Params: assetPath, key", "remove_stringtable_entry", (p) => ({ assetPath: p.assetPath, path: p.path, key: p.key })),
    import_stringtable:   bp("Import StringTable entries from CSV. Params: assetPath, filePath (or csvPath)", "import_stringtable", (p) => ({ assetPath: p.assetPath, path: p.path, filePath: p.filePath ?? p.csvPath })),
    import_stringtable_csv: bp("Import or refresh a String Table from a CSV that is the canonical source, using Unreal's own String Table CSV importer. The CSV is parsed into a throwaway table and checked against expectedKeys FIRST, so a malformed file or a key set that does not match leaves the asset untouched and comes back with missingKeys/unexpectedKeys instead. replaceExisting=true prunes entries the CSV no longer carries, which is what makes the CSV canonical rather than additive; the prune runs after a successful merge so a bad parse can never empty the table. Returns addedKeys, updatedKeys, removedKeys, entry counts and an explicit persisted/saved plus persistError, so a write that did not reach disk is never reported as a success. A relative csvPath is read against the project directory. Params: assetPath, csvPath (or filePath), expectedKeys? (string[]), requireExactKeys? (default false), replaceExisting? (default false), save? (default true) (#978)", "import_stringtable_csv", (p) => ({ assetPath: p.assetPath ?? p.path, csvPath: p.csvPath ?? p.filePath, expectedKeys: p.expectedKeys, requireExactKeys: p.requireExactKeys, replaceExisting: p.replaceExisting, save: p.save })),
    add_input_mapping:    bp("Append an Enhanced Input key mapping to an InputMappingContext (InputAction + key by name string e.g. 'Mouse2D','LeftMouseButton'). Idempotent on (action,key). For modifiers/triggers use gameplay(set_mapping_modifiers). Same as gameplay(add_imc_mapping) (#525). Params: mappingContext (IMC path), inputAction (IA path), key", "add_imc_mapping", (p) => ({ imcPath: p.mappingContext ?? p.imcPath ?? p.assetPath, inputActionPath: p.inputAction ?? p.inputActionPath, key: p.key })),
    remove_input_mapping: bp("Remove an IMC key mapping. Same as gameplay(remove_imc_mapping) (#525). Params: mappingContext (IMC path), mappingIndex? | (inputAction? + key?)", "remove_imc_mapping", (p) => ({ imcPath: p.mappingContext ?? p.imcPath ?? p.assetPath, mappingIndex: p.mappingIndex, inputActionPath: p.inputAction ?? p.inputActionPath, key: p.key })),
    list_input_mappings:  bp("List an IMC's key->action bindings with triggers/modifiers. Same as gameplay(read_imc) (#525). Params: mappingContext (IMC path)", "read_imc", (p) => ({ imcPath: p.mappingContext ?? p.imcPath ?? p.assetPath })),
    add_socket:           bp("Add socket to StaticMesh or SkeletalMesh. SkeletalMesh writes mesh-local sockets by default; pass a Skeleton asset path to edit skeleton-level sockets. Idempotent on socket name; pass onConflict='update' to overwrite an existing socket's transform with the supplied relativeLocation/relativeRotation/relativeScale (#412). Params: assetPath, socketName, boneName? (SkeletalMesh only, default 'root'), relativeLocation?, relativeRotation?, relativeScale?, onConflict? (skip\\|update\\|error, default skip)", "add_socket"),
    remove_socket:        bp("Remove socket by name. Params: assetPath, socketName", "remove_socket"),
    list_sockets:         bp("List sockets on a mesh (StaticMesh or SkeletalMesh). SkeletalMesh results include mesh-local sockets plus assigned Skeleton sockets, each with source='mesh' or source='skeleton'. Params: assetPath", "list_asset_sockets", (p) => ({ assetPath: p.assetPath })),
    set_socket_transform: bp("Update an existing socket's relative transform on StaticMesh or SkeletalMesh. Pass any subset of relativeLocation/relativeRotation/relativeScale; omitted fields stay at their current values. Errors if the socket does not exist (use add_socket to create). Common after FBX import when SOCKET_* empties land with scale=(100,100,100) (#412). Params: assetPath, socketName, relativeLocation?, relativeRotation?, relativeScale?", "set_socket_transform"),
    set_property:         bp("Set a UPROPERTY on any loaded asset (Material, DataAsset, DataTable, SubsurfaceProfile, etc.) using a dotted path. Blueprint paths resolve to the generated-class CDO so you can author its defaults + Instanced sub-object arrays (#568). Walks nested structs, array elements by index, and instanced subobjects internally - no more read-modify-write copies (e.g. `settings.mean_free_path_distance` on a UMaterial, or `Config.Traits[1].Params.Field` on a config asset #527). Value goes through MCPJsonProperty::SetJsonOnProperty so JSON null clears object refs, structs accept {x,y,z}, arrays/maps round-trip. TMap values take { \"Key\": value } or, for struct keys, [{ key: {...}, value: ... }]; a write that cannot store every entry fails and leaves the old value untouched (#820). The write is saved to the package by default and the result reports persisted plus the package name; a write that could not reach disk (save=false, a protected mount, a read-only file, a refused save) comes back with persisted=false and persistError naming the reason instead of a bare success that reverts on the next editor start (#931). Params: assetPath, propertyName (dotted path), value, save? (default true) (#420)", "set_asset_property", (p) => ({ assetPath: p.assetPath ?? p.path, propertyName: p.propertyName, value: p.value, save: p.save })),
    append_array_elements: bp("Append one or more JSON values to a reflected TArray without replacing existing entries. Supports dotted property paths plus native and user-defined USTRUCT elements. All elements are validated before mutation; returns appended indices and rollback data. The append is saved to the package by default and reports persisted / persistError the same way set_property does (#931). Params: assetPath, propertyName, elements, save? (default true)", "append_asset_array_elements", (p) => ({ assetPath: p.assetPath ?? p.path, propertyName: p.propertyName, elements: p.elements, save: p.save })),
    bulk_set_properties:  bp("Set dotted UPROPERTY paths on as many as 500 assets in one preflighted batch. Every asset, path, and value is validated before anything is mutated, and every submitted item comes back with its own ok/status/error, so a bad path in item 300 never hides the other 499 verdicts. Default is all-or-nothing: any preflight rejection aborts before a single UObject is touched. Pass continueOnError to apply the items that did pass and keep the rejects reported alongside them. Returns per-property readback, aggregate counts, targeted save results, and a replayable rollback payload covering only the writes that landed. Params: items ([{assetPath, properties}]), save? (default true), dryRun? (default false), continueOnError? (default false)", "bulk_set_asset_properties", (p) => ({ items: p.items, save: p.save, dryRun: p.dryRun, continueOnError: p.continueOnError })),
    bulk_read_properties: {
      description: "Read the SAME properties off many assets in one call, filtered and aggregated in the editor. read_properties answers one asset per call, which turns a library-wide question (concurrency settings across 582 sound assets, cull distances across 201 foliage types) into a loop. Select with assetPaths[] or directory + classNames[]; propertyNames accepts dotted paths into nested structs (e.g. 'CullDistance.Max'), and a Blueprint path reads its generated-class CDO. Predicates, groupBy and countBy all evaluate here: filtering happens before anything crosses the wire. Absent and null are reported separately, because 'this class has no such property' and 'this property is unset' are different findings; suspect is true for either, and suspectOnly returns just those rows. Never writes, and reports dirtiedPackages. Params: assetPaths? (string[]) OR directory? (+ recursive?, default true), classNames? (string[]), matchSubclasses? (default true), propertyNames (string[], required, max 32), where? ([{field, op, value}] over props.<name>, className, suspect; same operators as level(query_components)), whereMode? (all|any), suspectOnly?, groupBy?, countBy? (string[]), sampleLimit?, countOnly?, limit? (default 200, max 2000), startIndex?, maxAssets? (default 2000, max 20000), outputPath? (write every matched row to a JSON file and return the path instead of the rows) (#909)",
      bridge: "bulk_read_asset_properties",
      timeoutMs: 300_000,
      mapParams: (p) => ({
        assetPaths: p.assetPaths, directory: p.directory, recursive: p.recursive,
        classNames: p.classNames, matchSubclasses: p.matchSubclasses,
        propertyNames: p.propertyNames, where: p.where, whereMode: p.whereMode,
        suspectOnly: p.suspectOnly, groupBy: p.groupBy, countBy: p.countBy,
        sampleLimit: p.sampleLimit, countOnly: p.countOnly,
        limit: p.limit, startIndex: p.startIndex, maxAssets: p.maxAssets,
        outputPath: p.outputPath,
      }),
    },
    set_texture_settings_by_type: bp("Apply the canonical (compressionSettings, sRGB, LOD group) combo to every texture in each group: normal -> Normalmap, grayscale -> Grayscale, baseColor -> Default sRGB, hdr -> HDR. Params: groups (object: {normal?:[paths], grayscale?:[paths], baseColor?:[paths], hdr?:[paths]}) (#421)", "set_texture_settings_by_type", (p) => ({ groups: p.groups })),
    create_interchange_pipeline: bp("One-call factory for a UInterchangeGenericAssetsPipeline asset with the 15-property mesh-import boilerplate already applied (RecomputeNormals=false, MikkTSpace=true, HighPrecisionTangents=true, BuildNanite=false, CreatePhysicsAsset=false, etc.). Params: assetPath OR (name + packagePath?), meshType? (skeletal default | static), options? (dotted-path overrides on the resulting pipeline e.g. {'MeshPipeline.bBuildNanite': true}), onConflict? (#421)", "create_interchange_pipeline", (p) => ({ assetPath: p.assetPath, name: p.name, packagePath: p.packagePath, meshType: p.meshType, options: p.options, onConflict: p.onConflict })),
    reload_package:       bp("Force reload an asset package from disk. Params: assetPath", "reload_package"),
    health_check:         bp("Diagnose stuck-unloadable asset. Returns onDisk/inRegistry/isLoaded/canLoad/isStuck flags so an agent can detect the half-shutdown state where load returns null but the file exists (#279). Params: assetPath", "asset_health_check"),
    force_reload:         bp("Aggressive reload from disk: closes open editors, reloads the package (rebuilding a Blueprint's class and CDO so container properties come back fresh, not just scalars), and reports objectReplaced. Refuses a dirty package unless discardUnsaved=true, and fails loudly when the editor would not release the old object rather than serving stale values (#279/#820). Params: assetPath, discardUnsaved? (default false)", "force_reload_asset", (p) => ({ assetPath: p.assetPath ?? p.path, discardUnsaved: p.discardUnsaved })),
    export:               bp("Export asset to disk file (Texture2D → PNG, StaticMesh → FBX, etc.). Params: assetPath, outputPath", "export_asset"),
    search_fts:           bp("Ranked asset search (token-scored over name/class/path). Params: query, maxResults?, classFilter?", "search_assets_fts", (p) => ({ query: p.query, maxResults: p.maxResults, classFilter: p.classFilter })),
    reindex_fts:          bp("Rebuild the SQLite FTS5 asset index. Params: directory?", "reindex_assets_fts", (p) => ({ directory: p.directory })),
    get_referencers:      bp("Reverse dependency lookup (what references this). Params: packages[] OR packagePath (#150). Returns {referencersByPackage, totalReferencers}.", "get_asset_referencers", (p) => ({ packages: p.packages, packagePath: p.packagePath })),
    get_dependencies:     bp("Forward dependency lookup (what packages this asset references). Params: packages[] OR packagePath, hard? (default true), soft? (default true) (#588). Returns {dependenciesByPackage, totalDependencies}.", "get_asset_dependencies", (p) => ({ packages: p.packages, packagePath: p.packagePath, hard: p.hard, soft: p.soft })),
    list_skeleton_bones:  bp("List bones (names + rest-pose local and component-space transforms) from a SkeletalMesh or Skeleton asset, no live actor needed. Params: assetPath, includeTransforms? (default true) (#593). Returns {bones, boneCount, sourceKind}.", "list_skeleton_bones", (p) => ({ assetPath: p.assetPath, includeTransforms: p.includeTransforms })),
    get_primary_asset_ids: bp("Enumerate AssetManager-registered FPrimaryAssetIds (verify a primary-asset registration). Params: type? (FPrimaryAssetType; omit for all types), maxResults? (default 1000) (#579). Returns {primaryAssetIds:[{primaryAssetId, type, name, assetPath}], count, total}.", "get_primary_asset_ids", (p) => ({ type: p.type, maxResults: p.maxResults })),
    // v1.0.0-rc.2 - #155 (asset gaps)
    set_sk_material_slots: bp("Set materials on a USkeletalMesh by slot name or slotIndex (bypasses the blueprint override-materials path that UE's ICH silently reverts). Params: assetPath, slots[{slotName?|slotIndex?, materialPath}]", "set_sk_material_slots"),
    diagnose_registry:    bp("Scan a content path and compare disk vs AssetRegistry (including in-memory pending-kill entries). Returns onDiskCount, inMemoryIncludedCount, ghostCount and paths. Params: path, recursive? (default true), reconcile? (forceRescan=true)", "diagnose_registry"),
    get_mesh_bounds:      bp("Get StaticMesh OR SkeletalMesh bounding box. Params: assetPath. Returns min, max, boxExtent, boxCenter, meshKind (#193/#351)", "get_mesh_bounds"),
    get_mesh_info:        bp("One-call mesh QA: bounds + material slots + skeleton + LOD/vertex counts. Works for both UStaticMesh and USkeletalMesh. Params: assetPath. Returns meshKind, boundsOrigin, boundsExtent, heightM, lodCount, vertexCount, skeletonPath (skeletal only), materialSlots:[{index, slotName, materialPath, isDefaultFallback}], materialCount (#431)", "get_mesh_info"),
    read_import_sources:  bp("Read AssetImportData source filenames on an imported asset (StaticMesh, SkeletalMesh, Texture, Animation, etc.). Returns sources[] of {relativeFilename, absolutePath, timestamp, fileHash, displayLabelName}. Params: assetPath (#270)", "read_import_sources", (p) => ({ assetPath: p.assetPath ?? p.path })),
    get_mesh_collision:   bp("Inspect StaticMesh collision setup. Params: assetPath. Returns collisionTraceFlag, hasSimple/ComplexCollision, element counts (#177)", "get_mesh_collision"),
    get_mesh_geometry:    bp("Read actual vertex data off a StaticMesh OR SkeletalMesh: per-section positions, uvs, normals and triangle indices, from the engine's render data (no ProceduralMeshComponent plugin needed). Triangle indices are section-local, so they index that section's own positions array; add baseVertexIndex for LOD-global indices. Over 20000 vertices inline is refused - pass dumpToFile to write the full data to a JSON file instead, or narrow with sectionIndex. Params: assetPath, lodIndex? (default 0), sectionIndex? (omit for all sections), include? ([positions|uvs|normals|triangles], omit for all), uvChannel? (default 0), dumpToFile?, outputPath? (#948/#926/#953)", "get_mesh_geometry", (p) => ({ assetPath: p.assetPath ?? p.path, lodIndex: p.lodIndex, sectionIndex: p.sectionIndex, include: p.include, uvChannel: p.uvChannel, dumpToFile: p.dumpToFile, outputPath: p.outputPath })),
    measure_mesh_geometry: bp("Measure a StaticMesh OR SkeletalMesh LOD: bounds, dimensions, surfaceArea, volume, triangleCount, vertexCount, isClosed, isManifold, boundaryEdgeCount, nonManifoldEdgeCount. surfaceArea and volume are NAMED fields, never a positional pair (the engine's get_mesh_volume_area returns them in the order opposite to its name, #938). Vertices are welded by position before topology analysis so a UV seam does not read as a hole. Params: assetPath, lodIndex? (default 0), sectionIndex? (omit to measure the whole LOD) (#938/#953)", "measure_mesh_geometry", (p) => ({ assetPath: p.assetPath ?? p.path, lodIndex: p.lodIndex, sectionIndex: p.sectionIndex })),
    mesh_boolean:         { description: "Boolean CSG between two StaticMeshes: union, subtract, intersect, trimInside, trimOutside, newPolyGroupInside, newPolyGroupOutside. The target is the mesh being cut and the tool is what cuts it, each placed by its own optional transform. Writes to a SEPARATE output asset by default (outputPath, or '<targetPath>_<Operation>' when omitted); inPlace=true opts into overwriting the target and is the only destructive form. An existing outputPath is refused unless onConflict='replace'. An empty result is refused and nothing is written unless allowEmptyResult=true, because two meshes that never overlap otherwise report success having deleted everything. Returns triangle and vertex counts for both inputs and the result, plus the written asset's triangles, vertices, LOD count, material slots and bounds, so the operation can be verified rather than trusted. dryRun runs the boolean and reports those counts without writing. Materials and simple collision are copied from the target by default; nanite is inherit (match the target) | enable | disable. Needs the Geometry Script engine plugin: without it the call returns reason='geometry_scripting_unavailable' naming what to enable, rather than failing opaquely. Params: operation, targetPath, toolPath, outputPath?, inPlace?, targetTransform?, toolTransform?, lodType? (MaxAvailable|HiResSourceModel|SourceModel|RenderData), lodIndex?, fillHoles? (default true), simplifyOutput? (default true), simplifyPlanarTolerance? (default 0.01), allowEmptyResult?, recomputeNormals?, recomputeTangents?, removeDegenerates?, copyCollisionFromTarget? (default true), copyMaterialsFromTarget? (default true), nanite?, onConflict? (error|replace), dryRun?, save? (default true) (#916)", bridge: "mesh_boolean", mapParams: (p) => ({ operation: p.operation, targetPath: p.targetPath, toolPath: p.toolPath, outputPath: p.outputPath, inPlace: p.inPlace, targetTransform: p.targetTransform, toolTransform: p.toolTransform, lodType: p.lodType, lodIndex: p.lodIndex, fillHoles: p.fillHoles, simplifyOutput: p.simplifyOutput, simplifyPlanarTolerance: p.simplifyPlanarTolerance, allowEmptyResult: p.allowEmptyResult, recomputeNormals: p.recomputeNormals, recomputeTangents: p.recomputeTangents, removeDegenerates: p.removeDegenerates, copyCollisionFromTarget: p.copyCollisionFromTarget, copyMaterialsFromTarget: p.copyMaterialsFromTarget, nanite: p.nanite, onConflict: p.onConflict, dryRun: p.dryRun, save: p.save }) },
    migrate: {
      description:
        "Copy assets and their dependencies into ANOTHER project's Content directory - the scripted form of the content browser's Migrate (#760). " +
        "destinationContentDir is the TARGET project's Content folder. " +
        "While this server drives more than one editor, a 'toEditor' parameter is offered as well: name the destination editor and its Content folder is resolved for you and its asset registry rescanned afterwards, so the assets are visible there without a manual rescan (#817). " +
        "The call runs in the editor holding the SOURCE assets, so it pushes assets out of the project it is attached to. " +
        "Unsaved or never-saved assets are refused, because migrate copies files and would otherwise silently omit your edits. " +
        "Every asset is resolved before anything is copied, and the destination is checked for the packages afterwards rather than reporting success on the call returning. " +
        "Params: assetPaths (string[]) or assetPath, toEditor OR destinationContentDir, includeDependencies? (default true), onConflict? (skip|overwrite, default skip), allowDirty?, dryRun?",
      destinationEditor: true,
      handler: async (ctx, p) => migrateAssets(ctx, p),
    },
    move_folder:          bp("Move/rename entire content folder with redirector fixup in one transaction. Params: sourcePath, destinationPath (#192)", "move_folder"),
    create_folder:        bp("Create empty content browser folder(s). Params: path OR paths[] (e.g. /Game/Foo, /Game/Bar/Baz). Returns per-path created/existed/failed (#212)", "create_folder", (p) => ({ path: p.path, paths: p.paths })),
    delete_folder:        bp("Delete content browser folder(s) - counterpart to delete_asset, which leaves the parent directory entry behind as an orphan. Empty folders only by default; pass force=true to also delete any assets still inside (Content Browser 'Delete folder' equivalent). Per-path status (deleted/absent/failed) with reason (invalid_path/protected_path/not_empty/delete_failed) and a sample of contained assets on not_empty entries. Params: path OR paths[], force?", "delete_folder", (p) => ({ path: p.path, paths: p.paths, force: p.force })),
    set_mesh_nav:         bp("Set StaticMesh nav contribution. Params: assetPath, bHasNavigationData?, clearNavCollision? (#167)", "set_mesh_nav"),
    create_user_defined_enum: bp("Create a UserDefinedEnum content asset, optionally pre-populated with values. Params: name, packagePath? (default /Game), values? ([display-name strings]), onConflict? (#686)", "create_user_defined_enum", (p) => ({ name: p.name, packagePath: p.packagePath, values: p.values, onConflict: p.onConflict })),
    list_enum_values:     bp("List a UEnum's enumerators (index, authored short name, display name, value). Works on native and UserDefinedEnum assets. Params: assetPath (#686)", "list_enum_values", (p) => ({ assetPath: p.assetPath })),
    edit_user_defined_enum: bp("Author a UserDefinedEnum content asset. op=add_value appends an enumerator (authored name is auto-assigned; pass displayName - or name - to set the editable display text). op=rename_value sets a new displayName on the enumerator resolved by index or name (matches short or display name). op=remove_value deletes it. Recompiles dependents automatically. Native UEnums are not editable. Params: assetPath, op (add_value|rename_value|remove_value), displayName?, name?, index? (#686)", "edit_user_defined_enum", (p) => ({ assetPath: p.assetPath, op: p.op, displayName: p.displayName, name: p.name, index: p.index })),
    create_user_defined_struct: bp("Create a UserDefinedStruct content asset, optionally pre-populated with fields. Each field is {name, type} where type is a MakePinType string (bool|int|int64|float|string|name|text|byte, a struct like Vector, an enum, or an object ref like Actor). Params: name, packagePath? (default /Game), structFields? ([{name, type}]), onConflict? (#735)", "create_user_defined_struct", (p) => ({ name: p.name, packagePath: p.packagePath, fields: p.structFields, onConflict: p.onConflict })),
    list_struct_fields:   bp("List a UserDefinedStruct's members (index, internal name, friendly/display name, GUID, type label). Use this to find the GUID for a stable rename/retype. Native structs are not editable. Params: assetPath (#735)", "list_struct_fields", (p) => ({ assetPath: p.assetPath ?? p.path })),
    edit_user_defined_struct: bp("Author a UserDefinedStruct content asset. op=add_field appends a member (type via MakePinType string; pass fieldName for its display name). op=rename_field sets a new newDisplayName on the member resolved by fieldGuid or fieldName - the member GUID is preserved so existing Blueprint pins and DataTable rows survive. op=set_field_type changes a member's type. op=remove_field deletes it. Recompiles dependents automatically. Native structs are not editable. Params: assetPath, op (add_field|rename_field|set_field_type|remove_field), fieldName?, fieldGuid?, newDisplayName?, type? (#735)", "edit_user_defined_struct", (p) => ({ assetPath: p.assetPath ?? p.path, op: p.op, fieldName: p.fieldName, fieldGuid: p.fieldGuid, newDisplayName: p.newDisplayName, type: p.type })),
    rename_struct_field:  bp("Rename a UserDefinedStruct field's display name while preserving its member GUID, so Blueprint pins and DataTable rows keyed off it survive. Convenience wrapper over edit_user_defined_struct(op=rename_field). Resolve the field by fieldGuid or fieldName (matches friendly or internal name). Params: assetPath, fieldName | fieldGuid, newDisplayName (#735)", "edit_user_defined_struct", (p) => ({ assetPath: p.assetPath ?? p.path, op: "rename_field", fieldName: p.fieldName, fieldGuid: p.fieldGuid, newDisplayName: p.newDisplayName })),
    // Per-asset exclusive locking for concurrent agents. The lock registry
    // lives in the bridge (the shared editor), keyed by asset path with a TTL
    // so a crashed session never wedges an asset. sessionId defaults to this
    // server process; pass it explicitly to coordinate across processes.
    lock: {
      description: "Acquire an exclusive lock on an asset for this editor. Returns acquired=true, or acquired=false with holder{sessionId,ttlSecondsRemaining} when another session holds it. Params: assetPath, ttlSeconds? (default 300), sessionId?",
      handler: async (ctx, p) => ctx.bridge.call("acquire_lock", {
        path: p.assetPath ?? p.path,
        sessionId: lockOwner(ctx, p),
        ttlSeconds: p.ttlSeconds,
      }),
    },
    unlock: {
      description: "Release an asset lock held by this editor (or force=true to break any holder's lock). Params: assetPath, force?, sessionId?",
      handler: async (ctx, p) => ctx.bridge.call("release_lock", {
        path: p.assetPath ?? p.path,
        sessionId: lockOwner(ctx, p),
        force: p.force,
      }),
    },
    list_locks:           bp("List all currently-held asset locks with holder session id, acquiredAt, and ttlSecondsRemaining.", "list_locks"),
    unlock_all: {
      description: "Release every lock held by one session in a single call, returning the number released. Defaults to the addressed editor's own session; pass sessionId to clear a different one (for example after a crashed session left assets wedged). Params: sessionId?",
      handler: async (ctx, p) => ctx.bridge.call("release_session_locks", {
        sessionId: lockOwner(ctx, p),
      }),
    },
    diff:                 bp("Semantic structural diff between two assets, dispatching on the asset's class. Blueprints: parent class, variables, functions, components, per-graph node and connection deltas. Skeleton and SkeletalMesh: raw bone additions and removals, reparenting (bone, fromParent, toParent), raw-index changes (bone, fromIndex, toIndex), and declared virtual-bone additions and removals, with hierarchyCompatible and editorCompatible reported SEPARATELY. That separation is the point: it answers whether two skeletons are bone-compatible enough to register as Compatible Skeletons or whether a retarget is required, because appending bones (virtual ones especially) leaves the shared hierarchy intact while reparenting an existing bone does not (#879). Deliberately OUT of scope and reported as such: reference-pose transforms (referencePoseCompared=false), export names (exportNamesCompared=false), sockets and retarget sources; structureScope spells the boundary out in the result. Both paths must be the same class. Other asset types report that diffing is not supported yet rather than failing opaquely. Params: assetPath, otherPath", "diff_asset", (p) => ({ assetPath: p.assetPath ?? p.path, otherPath: p.otherPath })),
  },
  undefined,
  {
    save: z.boolean().optional().describe("set_property / append_array_elements / create_subobject / import_stringtable_csv / bulk_set_properties / bulk_upsert_data_assets / import_texture_batch / set_mesh_materials_batch / fixup_redirectors: write the changed package to disk (default true). false leaves the change in memory only and reports persisted=false with the reason (#931)."),
    saveMapPackages: z.boolean().optional().describe("save_all_dirty: include map packages (default true)"),
    saveContentPackages: z.boolean().optional().describe("save_all_dirty: include content packages (default true)"),
    items: z.array(z.union([
      z.object({
        filePath: z.string(),
        packagePath: z.string().optional(),
        name: z.string().optional(),
        replaceExisting: z.boolean().optional(),
      }),
      z.object({
        assetPath: z.string().min(1),
        properties: z.record(z.unknown()).refine((value) => Object.keys(value).length > 0, "properties must not be empty"),
      }),
      z.object({
        name: z.string(),
        packagePath: z.string(),
        className: z.string(),
        properties: z.record(z.unknown()).optional(),
      }),
    ])).min(1).max(500).optional().describe("Batch entries: import_texture_batch takes {filePath, packagePath?, name?, replaceExisting?}; bulk_set_properties takes {assetPath, properties}; bulk_upsert_data_assets takes {name, packagePath, className, properties?} (max 500)"),
    automated: z.boolean().optional().describe("import_texture_batch: bypass interactive dialogs (default true)"),
    assetPath: z.string().optional().describe("Asset path"),
    directory: z.string().optional(), query: z.string().optional(),
    maxResults: z.number().optional(), typeFilter: z.string().optional(),
    searchAll: z.boolean().optional().describe("Search all content roots (plugins, engine content) not just /Game/"),
    recursive: z.boolean().optional(),
    sourcePath: z.string().optional(), destinationPath: z.string().optional(),
    newName: z.string().optional(),
    materialPath: z.string().optional().describe("Material asset path for set_mesh_material"),
    slotIndex: z.number().optional().describe("Material slot index (default 0)"),
    filePath: z.string().optional().describe("Absolute file path for imports"),
    name: z.string().optional().describe("Asset name (defaults to filename)"),
    packagePath: z.string().optional().describe("Destination package path (e.g. /Game/Meshes)"),
    width: z.number().int().min(1).max(8192).optional().describe("create_render_target_2d: pixel width, 1-8192 (default 512)"),
    height: z.number().int().min(1).max(8192).optional().describe("create_render_target_2d: pixel height, 1-8192 (default 512)"),
    clearColor: z.object({
      r: z.number().optional(),
      g: z.number().optional(),
      b: z.number().optional(),
      a: z.number().optional(),
    }).optional().describe("create_render_target_2d: linear clear color (default transparent)"),
    generateMips: z.boolean().optional().describe("create_render_target_2d: automatically generate mipmaps (default false)"),
    targetGamma: z.number().min(0).optional().describe("create_render_target_2d: target gamma (default 0 uses engine behavior)"),
    onConflict: z.string().optional().describe("Asset-creation conflict policy: skip (default) | error | overwrite. bulk_upsert_data_assets uses update (default) | skip | error. mesh_boolean uses error (default) | replace"),
    groups: z.record(z.array(z.string())).optional().describe("set_texture_settings_by_type: { normal?: [...], grayscale?: [...], baseColor?: [...], hdr?: [...] }"),
    meshType: z.string().optional().describe("create_interchange_pipeline: 'skeletal' (default) or 'static'"),
    options: z.record(z.unknown()).optional().describe("create_interchange_pipeline: dotted-path overrides"),
    skeletonPath: z.string().optional(),
    combineMeshes: z.boolean().optional().describe("Combine all meshes in FBX into one (default false - imports as separate assets)"),
    importMaterials: z.boolean().optional(), importTextures: z.boolean().optional(),
    generateLightmapUVs: z.boolean().optional(),
    rowFilter: z.string().optional(), rowStruct: z.string().optional(),
    rowName: z.string().optional().describe("DataTable row key for set_datatable_row / remove_datatable_row"),
    row: z.record(z.unknown()).optional().describe("DataTable row fields object for set_datatable_row"),
    fields: z.record(z.unknown()).optional().describe("Alias for row in set_datatable_row"),
    data: z.record(z.unknown()).optional().describe("Alias for row in set_datatable_row"),
    mappingContext: z.string().optional().describe("InputMappingContext asset path for add_input_mapping (#525)"),
    inputAction: z.string().optional().describe("InputAction asset path for add_input_mapping (#525)"),
    imcPath: z.string().optional(),
    inputActionPath: z.string().optional(),
    key: z.string().optional().describe("Key name for add_input_mapping or StringTable entry key (e.g. 'Mouse2D', 'LeftMouseButton') (#525)"),
    sourceString: z.string().optional().describe("StringTable entry source string"),
    namespace: z.string().optional().describe("StringTable namespace for create_stringtable"),
    keyFilter: z.string().optional().describe("Filter StringTable keys by substring"),
    csvPath: z.string().optional().describe("StringTable CSV source path for import_stringtable / import_stringtable_csv. Relative paths are read against the project directory."),
    expectedKeys: z.array(z.string()).optional().describe("import_stringtable_csv: the keys the CSV must carry. Checked before the asset is touched; a mismatch returns missingKeys and changes nothing (#978)."),
    requireExactKeys: z.boolean().optional().describe("import_stringtable_csv: also fail when the CSV carries keys expectedKeys does not list (default false) (#978)."),
    mappingIndex: z.number().optional().describe("Index of an IMC mapping for remove_input_mapping (#525)"),
    fieldName: z.string().optional().describe("DataTable field/column name for set_datatable_cell (#535); also resolves a UserDefinedStruct member by friendly/internal name for edit_user_defined_struct/rename_struct_field, or names the new member for add_field (#735)"),
    oldName: z.string().optional().describe("Existing row key for rename_datatable_row (#535)"),
    rows: z.record(z.unknown()).optional().describe("DataTable bulk rows { rowName: {field: value} } for fill_datatable_from_json (#535)"),
    jsonPath: z.string().optional(), jsonString: z.string().optional(),
    csvString: z.string().optional().describe("CurveTable CSV payload for import_curvetable"),
    format: z.enum([
      "json", "csv",
      "R8", "RG8", "RGBA8", "RGBA8_SRGB",
      "R16F", "RG16F", "RGBA16F", "R32F", "RG32F", "RGBA32F", "RGB10A2",
    ]).optional().describe("CurveTable import format or create_render_target_2d pixel format"),
    interpMode: z.enum(["linear", "constant", "cubic", "none"]).optional().describe("CurveTable interpolation mode"),
    curveType: z.enum(["simple", "rich"]).optional().describe("CurveTable row type"),
    mode: z.enum(["simple", "rich"]).optional().describe("Alias for curveType"),
    keys: z.array(z.object({
      time: z.number(),
      value: z.number(),
      interpMode: z.enum(["linear", "constant", "cubic", "none"]).optional(),
      arriveTangent: z.number().optional(),
      leaveTangent: z.number().optional(),
    })).optional().describe("CurveTable key array"),
    time: z.number().optional().describe("CurveTable key time"),
    keyTimeTolerance: z.number().optional().describe("CurveTable key update tolerance"),
    exportName: z.string().optional(), propertyName: z.string().optional(),
    value: z.unknown().optional().describe("Property value for set_property - scalar, object/array, or asset-path string. Goes through MCPJsonProperty (#420/#531)"),
    elements: z.array(z.unknown()).min(1).optional().describe("append_array_elements: one or more values to append after full prevalidation"),
    includeValues: z.boolean().optional().describe("Include property values in read_properties/list_properties/get_properties"),
    continueOnError: z.boolean().optional().describe("bulk_set_properties / set_mesh_materials_batch: apply the items that passed preflight instead of aborting the whole batch (default false). Rejected items are still reported in items[]"),
    dryRun: z.boolean().optional().describe("migrate: resolve and report without copying (#760). bulk_upsert_data_assets: run the full preflight and report planned statuses without writing. fixup_redirectors: report the redirectors, referencers and packages it would load and save, and stop there (#908). mesh_boolean: run the boolean and report the result counts without writing an asset"),
    allowProjectWide: z.boolean().optional().describe("fixup_redirectors: permit a path that names a whole content root. Without it such a path is refused, so a targeted fix-up cannot become a project-wide resave (#908)."),
    discardUnsaved: z.boolean().optional().describe("force_reload: reload even though the package has unsaved changes, discarding them (default false) (#820)"),
    allowDirty: z.boolean().optional().describe("migrate: migrate the on-disk version of an asset with unsaved edits (#760)"),
    destinationContentDir: z.string().optional().describe("migrate: the TARGET project's Content folder (#760)"),
    includeDependencies: z.boolean().optional().describe("migrate: also copy referenced assets (default true) (#760)"),
    expandDepth: z.number().int().min(0).max(5).optional().describe("read_properties: inline owned subobjects to this depth (default 0) (#755)"),
    expandExternal: z.boolean().optional().describe("read_properties: also follow references to other assets (default false) (#755)"),
    maxExpandedObjects: z.number().int().positive().optional().describe("read_properties: cap on expanded objects (default 64) (#755)"),
    valueFormat: z.enum(["text", "json"]).optional().describe("Property value format for read_properties/list_properties/get_properties. Default text preserves the existing Unreal ExportText output; json returns structured JSON where supported."),
    settings: z.record(z.unknown()).optional(),
    compressionSettings: z.string().optional().describe("Texture compression: Default, Normalmap, Grayscale, Displacementmap, VectorDisplacementmap, HDR, EditorIcon, Alpha, DistanceFieldFont, HDR_Compressed, BC7"),
    lodGroup: z.string().optional().describe("Texture LOD group: World, WorldNormalMap, Character, UI, Lightmap, Effects, etc."),
    sRGB: z.boolean().optional(),
    neverStream: z.boolean().optional(),
    skeletalMeshPath: z.string().optional().describe("read_cloth_data/set_cloth_config: skeletal mesh path (#595)"),
    clothingAsset: z.string().optional().describe("set_cloth_config: clothing asset name filter (#595)"),
    configType: z.string().optional().describe("set_cloth_config: config class/key filter (#595)"),
    assetPathA: z.string().optional().describe("compare_textures: first texture (#697)"),
    assetPathB: z.string().optional().describe("compare_textures: second texture (#697)"),
    importUniformScale: z.number().optional().describe("import_skeletal_mesh: FBX uniform scale; pass 100 for metre-authored FBX (#687)"),
    importMorphTargets: z.boolean().optional().describe("import_skeletal_mesh: import morph targets (default true) (#678)"),
    createPhysicsAsset: z.boolean().optional().describe("import_skeletal_mesh: auto-create a PhysicsAsset (default false) (#678)"),
    replaceExisting: z.boolean().optional().describe("import_skeletal_mesh: replace an existing asset at the destination (default true) (#678)"),
    assetPaths: z.array(z.string()).optional().describe("Array of asset paths (recenter_pivot batch - first mesh sets reference pivot; also migrate and bulk_read_properties)"),
    // bulk_read_properties (#909)
    propertyNames: z.array(z.string()).optional().describe("bulk_read_properties: property paths to read off every matched asset; dotted paths walk nested structs (max 32)"),
    classNames: z.array(z.string()).optional().describe("bulk_read_properties: restrict to these asset classes"),
    matchSubclasses: z.boolean().optional().describe("bulk_read_properties: also match subclasses of classNames (default true)"),
    where: z.array(z.object({
      field: z.string().describe("Dot path into the row, e.g. props.CullDistance.Max, className or suspect"),
      op: z.string().optional().describe("eq (default), ne, lt, lte, gt, gte, contains, notContains, startsWith, endsWith, in, notIn, exists, notExists, isNull, isNotNull, isTrue, isFalse"),
      value: z.unknown().optional(),
    })).optional().describe("bulk_read_properties: predicates evaluated in the editor (max 24)"),
    whereMode: z.string().optional().describe("bulk_read_properties: all (default) or any"),
    suspectOnly: z.boolean().optional().describe("bulk_read_properties: only rows where a requested property is absent or null"),
    groupBy: z.string().optional().describe("bulk_read_properties: dot path to group matches by; returns key/count/samples (max 200 groups)"),
    countBy: z.array(z.string()).optional().describe("bulk_read_properties: dot paths to build value histograms for (max 8 paths, 64 values each)"),
    sampleLimit: z.number().optional().describe("bulk_read_properties: sample asset names per group (default 5, max 25)"),
    countOnly: z.boolean().optional().describe("bulk_read_properties: return aggregates without rows"),
    limit: z.number().optional().describe("bulk_read_properties: rows per page (default 200, max 2000)"),
    startIndex: z.number().optional().describe("bulk_read_properties: first row index for paging"),
    maxAssets: z.number().optional().describe("bulk_read_properties: refuse to load more than this many candidates (default 2000, max 20000)"),
    renames: z.array(z.record(z.unknown())).optional().describe("Array of rename descriptors for bulk_rename - each {sourcePath, destinationPath} or {assetPath, newName}"),
    socketName: z.string().optional().describe("Socket name"),
    boneName: z.string().optional().describe("Bone name (for skeletal mesh sockets)"),
    relativeLocation: Vec3.optional().describe("Socket relative location"),
    relativeRotation: Rotator.optional().describe("Socket relative rotation"),
    relativeScale: Vec3.optional().describe("Socket relative scale"),
    outputPath: z.string().optional().describe("Absolute file path for export (e.g. C:/output/texture.png); also the destination for get_mesh_geometry's dumpToFile (relative paths resolve under the project's Saved/)"),
    lodIndex: z.number().int().min(0).optional().describe("get_mesh_geometry / measure_mesh_geometry: which LOD to read (default 0). mesh_boolean: which LOD of each input mesh to read (default 0)"),
    sectionIndex: z.number().int().min(0).optional().describe("get_mesh_geometry / measure_mesh_geometry: restrict to one render section; omit for every section"),
    uvChannel: z.number().int().min(0).optional().describe("get_mesh_geometry: which UV channel to return (default 0)"),
    include: z.array(z.enum(["positions", "uvs", "normals", "triangles"])).optional().describe("get_mesh_geometry: which per-vertex arrays to return; omit for all four"),
    dumpToFile: z.boolean().optional().describe("get_mesh_geometry: write the full geometry JSON to a file instead of returning it inline, which is how a mesh over the inline vertex limit is read"),
    operation: z.string().optional().describe("mesh_boolean: union | subtract | intersect | trimInside | trimOutside | newPolyGroupInside | newPolyGroupOutside"),
    targetPath: z.string().optional().describe("mesh_boolean: the StaticMesh being cut. Its transform, materials, collision and Nanite setting are the ones the result inherits."),
    toolPath: z.string().optional().describe("mesh_boolean: the StaticMesh doing the cutting."),
    inPlace: z.boolean().optional().describe("mesh_boolean: overwrite targetPath instead of writing a separate output asset. Default false, and the only destructive form; mutually exclusive with outputPath."),
    targetTransform: z.object({ location: Vec3.optional(), rotation: Rotator.optional(), scale: Vec3.optional() }).optional().describe("mesh_boolean: where the target mesh sits for the boolean. Default identity."),
    toolTransform: z.object({ location: Vec3.optional(), rotation: Rotator.optional(), scale: Vec3.optional() }).optional().describe("mesh_boolean: where the tool mesh sits for the boolean. Default identity."),
    lodType: z.enum(["MaxAvailable", "HiResSourceModel", "SourceModel", "RenderData"]).optional().describe("mesh_boolean: which mesh data to read from each input. Default MaxAvailable, which ignores lodIndex."),
    fillHoles: z.boolean().optional().describe("mesh_boolean: close the holes the cut opens (default true)"),
    simplifyOutput: z.boolean().optional().describe("mesh_boolean: collapse coplanar triangles the boolean introduced (default true)"),
    simplifyPlanarTolerance: z.number().optional().describe("mesh_boolean: how far from coplanar still counts as coplanar when simplifying (default 0.01)"),
    allowEmptyResult: z.boolean().optional().describe("mesh_boolean: accept a result with no triangles. Default false, so two meshes that never overlap are refused rather than written as an empty asset."),
    recomputeNormals: z.boolean().optional().describe("mesh_boolean: recompute normals on the written mesh (default false)"),
    recomputeTangents: z.boolean().optional().describe("mesh_boolean: recompute tangents on the written mesh (default false)"),
    removeDegenerates: z.boolean().optional().describe("mesh_boolean: drop degenerate triangles when writing back into an existing mesh (default false)"),
    copyCollisionFromTarget: z.boolean().optional().describe("mesh_boolean: copy the target's simple collision shapes and collision complexity onto the result (default true)"),
    copyMaterialsFromTarget: z.boolean().optional().describe("mesh_boolean: copy the target's material slot list onto the result (default true)"),
    nanite: z.enum(["inherit", "enable", "disable"]).optional().describe("mesh_boolean: Nanite on the written mesh. inherit (default) matches the target."),
    classFilter: z.string().optional().describe("Restrict search_fts to assets whose class name contains this substring"),
    className: z.string().optional().describe("Class for create_data_asset/create_asset_by_class: loaded class name with or without the C++ A/U/F/E prefix, or a /Script/Module.ClassName path"),
    properties: z.record(z.unknown()).optional().describe("Key/value property overrides for create_data_asset and create_subobject. Keys may be dotted paths into nested structs and subobjects."),
    outer: z.enum(["asset", "package"]).optional().describe("create_subobject: whether the new subobject is owned by the asset (default, path \"<asset>.<name>\") or sits beside it in the package (path \"<package>.<name>\") (#975)."),
    packages: z.array(z.string()).optional().describe("Package paths for get_referencers / get_dependencies"),
    hard: z.boolean().optional().describe("get_dependencies: include hard dependencies (default true)"),
    soft: z.boolean().optional().describe("get_dependencies: include soft dependencies (default true)"),
    includeTransforms: z.boolean().optional().describe("list_skeleton_bones: include rest-pose transforms (default true)"),
    type: z.string().optional().describe("get_primary_asset_ids: FPrimaryAssetType filter (omit for all types) (#579); also the member type (MakePinType string) for edit_user_defined_struct add_field/set_field_type (#735)"),
    // #155
    slots: z.array(z.object({
      slotName: z.string().optional(),
      slotIndex: z.number().optional(),
      materialPath: z.string(),
    })).optional().describe("Per-slot material assignments for set_sk_material_slots"),
    // #822
    assignments: z.array(z.object({
      assetPath: z.string().min(1),
      materialPath: z.string().min(1),
      slotName: z.string().optional(),
      slotIndex: z.number().int().optional(),
    })).min(1).max(500).optional().describe("set_mesh_materials_batch entries: [{assetPath, materialPath, slotName? | slotIndex?}] (max 500). slotName is preferred for imported kits because slot indices are not stable across reimports"),
    path: z.string().optional().describe("Content path (e.g. /Game/Foo) - used by diagnose_registry, create_folder"),
    op: z.string().optional().describe("edit_user_defined_enum op: add_value | rename_value | remove_value. edit_user_defined_struct op: add_field | rename_field | set_field_type | remove_field"),
    values: z.array(z.string()).optional().describe("create_user_defined_enum: initial value display names"),
    displayName: z.string().optional().describe("edit_user_defined_enum: display text for the enumerator"),
    index: z.number().optional().describe("edit_user_defined_enum: enumerator index for rename/remove"),
    structFields: z.array(z.object({
      name: z.string(),
      type: z.string().optional(),
    })).optional().describe("create_user_defined_struct: initial members as [{name, type}] (type is a MakePinType string, default bool)"),
    fieldGuid: z.string().optional().describe("edit_user_defined_struct/rename_struct_field: resolve a field by its member GUID (stable across renames)"),
    newDisplayName: z.string().optional().describe("edit_user_defined_struct/rename_struct_field: new display name for rename_field"),
    paths: z.array(z.string()).optional().describe("Multiple content paths for create_folder. fixup_redirectors: the redirector packages, or the folders holding them, to fix up (#908)"),
    reconcile: z.boolean().optional().describe("diagnose_registry: force synchronous rescan (evicts pending-kill ghosts)"),
    bHasNavigationData: z.boolean().optional().describe("Toggle nav data generation for set_mesh_nav"),
    clearNavCollision: z.boolean().optional().describe("Remove NavCollision from mesh for set_mesh_nav"),
    offset: z.number().optional().describe("list: index of the first match to return, for paging large folders (#790)"),
    force: z.boolean().optional().describe("delete / delete_batch: take the force-delete path, which destroys an asset even when other packages reference it and auto-closes any open asset editors (#278). Defaults to false, which checks the Asset Registry for referencers first and refuses when it finds any (#976). delete_folder: also delete assets contained in the folder. save: write even if the package is not marked dirty (#768)."),
    otherPath: z.string().optional().describe("diff: the asset to compare assetPath against"),
    // lock / unlock / unlock_all all default this to the server process's own
    // session id; it is only passed explicitly to coordinate across processes.
    sessionId: z.string().optional().describe("lock / unlock / unlock_all: owning session id (defaults to this server process)"),
    ttlSeconds: z.number().optional().describe("lock: seconds before the lock auto-expires (default 300)"),
  },
);
