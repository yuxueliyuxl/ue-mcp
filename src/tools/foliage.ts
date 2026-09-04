import { z } from "zod";
import { categoryTool, bp, type ToolDef } from "../types.js";
import { Vec3 } from "../schemas.js";

export const foliageTool: ToolDef = categoryTool(
  "foliage",
  "Foliage painting, types, sampling, and settings.",
  {
    list_types:    bp("List foliage types in level", "list_foliage_types"),
    get_settings:  bp("Read foliage type settings. Params: foliageTypeName", "get_foliage_type_settings"),
    sample:        bp("Query instances in region. Params: center, radius, foliageType?", "sample_foliage"),
    create_type:   bp("Create foliage type from mesh. Params: meshPath, name?, packagePath?", "create_foliage_type"),
    set_settings:  bp("Modify ONE foliage type's settings. For the same edit across every type matching a condition, use batch_set_settings_where instead of calling this in a loop. Params: foliageTypeName OR foliageTypePath, settings", "set_foliage_type_settings"),
    batch_set_settings_where: {
      description: "Write settings to every FoliageType whose CURRENT property values match a predicate, with the predicate evaluated in the editor. set_settings is one asset per call and cannot express a condition at all, so 'include_in_hlod=false on every type whose cull distance is set' meant reading 201 assets out, computing the 36 matches client-side, and writing them back one at a time. Candidates come from foliageTypePaths[] (explicit), directory (scan FoliageType assets) or fromLevel=true (types placed in the open level). Predicate fields read the asset's own properties by dotted path, so CullDistance.Max works and a bare name is treated the same as props.<name>; operators are the same vocabulary as level(query_components). dryRun DEFAULTS TO TRUE because this writes to many assets at once, and the preview still resolves every setting path so an unwritable property is reported before the commit. Written values are read back rather than echoed, so a value the property coerced is visible. Params: settings (object of propertyName -> value, dotted paths supported), where ([{field, op, value}], required), whereMode? (all|any), one of foliageTypePaths[]/directory (+ recursive?)/fromLevel, propertyNames? (extra properties to report without filtering on them), dryRun? (default TRUE), save? (default true), maxTypes? (#988)",
      bridge: "batch_set_foliage_settings_where",
      timeoutMs: 300_000,
      mapParams: (p) => ({
        settings: p.settings, where: p.where, whereMode: p.whereMode,
        foliageTypePaths: p.foliageTypePaths, directory: p.directory, recursive: p.recursive,
        fromLevel: p.fromLevel, propertyNames: p.propertyNames,
        dryRun: p.dryRun, save: p.save, maxTypes: p.maxTypes,
      }),
    },
  },
  undefined,
  {
    foliageTypePath: z.string().optional().describe("Full FoliageType asset path"),
    foliageTypePaths: z.array(z.string()).optional().describe("batch_set_settings_where: explicit FoliageType asset paths (#988)"),
    directory: z.string().optional().describe("batch_set_settings_where: content directory to scan for FoliageType assets (#988)"),
    recursive: z.boolean().optional().describe("batch_set_settings_where: scan directory recursively (default true)"),
    fromLevel: z.boolean().optional().describe("batch_set_settings_where: take the foliage types placed in the open level (#988)"),
    where: z.array(z.object({
      field: z.string().describe("Dot path into the asset's own properties, e.g. CullDistance.Max or props.CullDistance.Max"),
      op: z.string().optional().describe("eq (default), ne, lt, lte, gt, gte, contains, notContains, startsWith, endsWith, in, notIn, exists, notExists, isNull, isNotNull, isTrue, isFalse"),
      value: z.unknown().optional(),
    })).optional().describe("batch_set_settings_where: predicate over each type's current values, evaluated in the editor (#988)"),
    whereMode: z.string().optional().describe("batch_set_settings_where: all (default) or any"),
    propertyNames: z.array(z.string()).optional().describe("batch_set_settings_where: extra properties to report on each row without filtering on them"),
    dryRun: z.boolean().optional().describe("batch_set_settings_where: preview without writing (defaults to TRUE)"),
    save: z.boolean().optional().describe("batch_set_settings_where: save each changed FoliageType package (default true)"),
    maxTypes: z.number().optional().describe("batch_set_settings_where: refuse to scan more than this many types (default 2000)"),
    foliageTypeName: z.string().optional(),
    foliageType: z.string().optional(),
    center: Vec3.optional(),
    radius: z.number().optional(),
    count: z.number().optional(),
    density: z.number().optional(),
    meshPath: z.string().optional(),
    name: z.string().optional(),
    packagePath: z.string().optional(),
    settings: z.record(z.unknown()).optional(),
  },
);
