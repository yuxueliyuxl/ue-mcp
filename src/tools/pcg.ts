import { z } from "zod";
import type { ToolDef } from "../core/types.js";
import { categoryTool } from "../surface/category-tool.js";
import { waitAfterRegenerate, waitForGeneration } from "./pcg-wait.js";
import { actions as epicActions, schema as epicSchema } from "./epic/pcg.generated.js";
import { specBp, schema as specSchema } from "./specs/pcg.generated.js";

export const pcgTool: ToolDef = categoryTool(
  "pcg",
  "Procedural Content Generation: graphs, nodes, connections, execution, volumes.",
  {
    list_graphs:          specBp("read", "List PCG graphs, sorted by object path. Lists the whole project; page through it with cursor/limit.", "list_pcg_graphs"),
    read_graph:           specBp("read", "Read graph structure.", "read_pcg_graph"),
    read_node_settings:   specBp("read", "Read node settings. An instanced subobject (a spawner's MeshSelectorParameters or InstanceDataPackerParameters) is expanded under instanced {class, properties}, and a weighted mesh selector also lists meshEntries [{mesh, weight, materialOverrides}] (#1303).", "read_pcg_node_settings"),
    get_components:       specBp("read", "List PCG components in level.", "get_pcg_components"),
    get_component_details: specBp("read", "Inspect PCG component (#983). Reports isGenerating, isCleaningUp, generated, partitioned (with local component counts) and lastRun {id, state generating|generated|cancelled|cleaned|not_started, startedUtc, finishedUtc}; to wait for a run, use wait_for_generation or wait=true on execute.", "get_pcg_component_details"),
    create_graph:         specBp("mutate", "Create graph in packagePath (or path), default /Game/PCG. Idempotent by path: an existing graph is reported rather than replaced.", "create_pcg_graph"),
    add_node:             specBp("mutate", "Add node. The engine assigns its name unless name is given; either way nodeName in the result is what connect_nodes and remove_node take. A name already in the graph with the same class is reported as existed; another class is refused (#1331).", "add_pcg_node"),
    connect_nodes:        specBp("mutate", "Wire nodes. Returns edgeVerified=true after confirming the UPCGEdge persisted; surfaces an error if AddEdge succeeded but no edge object was instantiated (#304).", "connect_pcg_nodes"),
    disconnect_nodes:     specBp("mutate", "Remove a wired edge between two PCG nodes. Omitted pins match any pin. Returns removedEdges count (#346).", "disconnect_pcg_nodes"),
    set_node_settings:    specBp("mutate", "Set node params. Pass a settings object of {propertyPath: value} (dotted paths and nested structs supported), or propertyName + propertyValue for a single write. Reports previousProperties and rolls back to them. Writing a spawner's MeshSelectorType or InstanceDataPackerType recreates its selector or packer (listed in recreatedInstancedParameters); a path for an instanced subobject is refused. Addresses the graph's DefaultInputNode and DefaultOutputNode too: writing their Pins changes the graph's own pins and those of every Subgraph node calling it. An attribute selector (TargetAttribute, InputSource1, OutputTarget...) takes the PCG editor's syntax as a string: MyAttr, $Density, @Last, $Position.Z (#1275); a selector given as an object replaces the whole selector, so fields left out (ExtraNames) reset. Returns resolvedSelectors with each written selector as stored, and removedEdges for edges a pin-changing write dropped, with a warning (#1298). readBack holds each written property as stored; struct text that left fields out (which keep their old values) adds a warning naming the property (#1299).", "set_pcg_node_settings"),
    set_static_mesh_spawner_meshes: specBp("mutate", "Populate weighted MeshEntries on a PCGStaticMeshSpawner node (#145). entries=[{mesh, weight?}]; replace defaults to true. Refuses a selector the node's settings do not own; reports selectorOuterIsSettings.", "set_static_mesh_spawner_meshes"),
    remove_node:          specBp("mutate", "Remove node. The graph's input and output nodes are refused.", "remove_pcg_node"),
    unwrap_instance_nodes: specBp("mutate", "Give instance nodes (UPCGSettingsInstance wrappers, read-only in the PCG editor's details panel) their own settings object, keeping every value and edge. Settings from a shared asset are copied into the node. Nodes that already own their settings are left alone. Omit nodeName for every node in the graph (#1087).", "unwrap_pcg_instance_nodes"),
    execute: {
      ...specBp("mutate", "Regenerate PCG. A seed is written to the component before generating, and the old one reported back as previousSeed (#983). Returns at once with runId, or with wait=true only once that run finishes, adding wait {state generated|cancelled|cleaned|not_started|timed_out, durationMs}; a timed-out wait is a warning, not a failure (#1365). Refused with generation_in_progress while a run is active.", "execute_pcg_graph"),
      followUp: waitAfterRegenerate,
    },
    force_regenerate: {
      ...specBp("mutate", "Force a stuck PCG component to regenerate (clears graph ref, re-sets, cleanup+generate) (#146/#983). Cancels active generation before resetting; returns runId like execute, including when the component was busy, and takes wait=true the same way (#1365).", "force_regenerate_pcg"),
      followUp: waitAfterRegenerate,
    },
    get_generation_state: specBp("read", "One poll of a PCG component's generation: busy (generating, cleaning up, or local components generating), finishCount and lastFinishState for generations that ended since the bridge started watching the component (whoever started them), and lastRun. wait_for_generation polls this for you (#1365).", "get_pcg_generation_state"),
    wait_for_generation: {
      kind: "handler",
      effect: "read",
      description: "Block until a PCG component's generation finishes, for a run started by anything (execute, the editor, a project function that defers to the next tick), polling get_generation_state with backoff. With runId, waits for that run; without, for the generation in flight, or one that starts within graceSeconds (default 2), else returns state idle. Partitioned components finish when their local components do. Returns state generated|cancelled|cleaned|not_started|idle|timed_out, durationMs, polls and lastRun; timed_out fails the call (#1365). Params: actorLabel?, actorPath?, componentName?, runId?, timeoutSeconds?, graceSeconds?",
      handler: async (ctx, p) => waitForGeneration(ctx, {
        actorLabel: p.actorLabel as string | undefined,
        actorPath: p.actorPath as string | undefined,
        componentName: p.componentName as string | undefined,
        runId: p.runId as number | undefined,
        timeoutSeconds: p.timeoutSeconds as number | undefined,
        graceSeconds: p.graceSeconds as number | undefined,
      }),
    },
    cleanup:              specBp("mutate", "Cleanup a PCG component (remove spawned content) (#146).", "cleanup_pcg"),
    toggle_graph:         specBp("mutate", "Toggle a PCG component's graph assignment to force reinit (no generate) (#146).", "toggle_pcg_graph"),
    add_volume:           specBp("mutate", "Place PCG volume. Idempotent by editor label when one is given. transform {location, rotation, scale|scale3D} is honoured; reports the placed location, rotation, scale, extent and scaledExtent.", "add_pcg_volume"),
    import_graph:         specBp("mutate", "Bulk-author a PCG graph from JSON: nodes=[{name,class,posX?,posY?,settings?,subgraph?:{path,parameters}}], connections=[{from,fromPin?,to,toPin?}], replace defaults to false. Without replace, a name the graph already has is that node: the same class updates it in place (counted in nodesUpdated and listed in updatedNodes) and edges naming it land on it; another class, or a name given twice, refuses the call before anything changes (#1262). removeNodes and removeConnections run first, without replace. The rollback of an import without replace removes the nodes it created, cuts the edges it added and restores the values it overwrote; only replace=true carries a full graph snapshot (#1331). One call replaces N add_node + M connect_nodes + K set_node_settings (#213). A path for an instanced subobject is refused before anything changes; failed nodes, settings or connections make the result a failure; name-collision warnings are informational. Attribute selectors in settings take the PCG editor's syntax as a string ($Position.Z, @Last). A Get Graph Parameter node (PCGUserParameterGetSettings) takes parameter: '<name>' and binds to that graph parameter; export_graph emits it the same way (#1264).", "import_pcg_graph"),
    export_graph:         specBp("read", "Export a PCG graph as JSON; includeSettings defaults to true. Round-trip safe with import_graph (#213). Subgraph assignments and parameter overrides are preserved as subgraph {path,parameters}; other instanced subobjects (a spawner's mesh selector) are left out and listed in omittedInstancedProperties. Attribute selectors export in editor syntax (\"$Position.Z\"), extra names included (#1267).", "export_pcg_graph"),
    export_level_to_asset: specBp("mutate", "Export a saved level to a PCG data asset (a PCG Assembly), the Content Browser's 'Create PCG Assets from Level(s)' without its dialog. Default name <LevelName>_PCG beside the level; an existing asset is overwritten. assetPath takes the asset's own path (object path, or an existing asset's package path) as well as a folder for assetName, and an existing asset of another class is refused (#1296). Reports points per output pin (Root, Points). UE 5.5+ (#1244).", "export_level_to_pcg_asset"),
    update_level_assets:  specBp("mutate", "Re-export PCG data assets from the levels they were exported from ('Update PCG Assets'). Reports points per output pin. UE 5.5+ (#1244).", "update_pcg_level_assets"),
    set_subgraph:         specBp("mutate", "Point a Subgraph node at a PCGGraph or PCGGraphInstance through the engine's SetSubgraph, so its pins and user-parameter bag are built (a write to SubgraphInstance.Graph builds pins but no parameters). \"\" clears it. Returns the node's pins and every parameter with its value and override state (#1253).", "set_pcg_subgraph"),
    set_subgraph_parameters: specBp("mutate", "Override a Subgraph node's user parameters: parameters={name: value}; null clears an override. A name the subgraph does not declare, or a value of the wrong type, refuses the whole call with nothing changed. Returns previous values and rolls back to them (#1253).", "set_pcg_subgraph_parameters"),
    set_graph_pins:       specBp("mutate", "Add, update, replace or remove the graph's own pins on its input or output node (side), matched by label: append (default) adds a pin or updates the one with that label, replace makes the pins exactly the list, remove deletes the labels given. Fields left out keep an existing pin's values, so its edges stay wired. Every Subgraph node calling the graph gets the same pins. Returns the resulting pins and removedEdges for edges a removed or retyped pin dropped. Rolls back to the previous Pins through set_node_settings (#1318).", "set_pcg_graph_pins"),
    set_graph_parameters: specBp("mutate", "Add, change, retype or remove a graph's user parameters (the Graph Parameters a Get Graph Parameter node and a Subgraph or Graph Instance override read). graphParameters=[{name, type?, typeObject?, default?}]: a new name needs a type; an existing one keeps its type unless another is given; default is JSON or export text. removeParameters=[names]. Every entry is checked on a scratch copy first, so one bad entry refuses the call with nothing changed. Returns the parameters and previousParameters; the rollback removes what was added and restores changed defaults (#1264).", "set_pcg_graph_parameters"),
    create_graph_instance: specBp("mutate", "Create a PCG Graph Instance asset running graphPath, with optional parameter overrides={name: value}; an override for a name the graph does not declare, or a value of the wrong type, sets none and fails with the asset path in createdWithoutOverrides. Idempotent by path. Returns every parameter with its value and override state (#1264).", "create_pcg_graph_instance"),
    set_graph_instance_overrides: specBp("mutate", "Set a PCG Graph Instance asset's parameter overrides: overrides={name: value}; null clears one. A name the graph does not declare, or a value of the wrong type, refuses the whole call with nothing changed. Returns previous values and rolls back to them (#1264).", "set_pcg_graph_instance_overrides"),
    read_generated_output: specBp("read", "Read the data a PCG component generated, for any actor's PCG components (not only PCGVolumes): per tagged data its pin, tags, dataClass, kind (points | attributeSet | other), count, and a page of entries with the requested attributes (selectors such as $Position, $Density, $Position.Z or a metadata attribute; default the point properties plus every metadata attribute). offset/limit page the entries, and nextOffset says there are more. A partitioned component's local components are read too. With nodeName it reads that node's output from the component's inspection instead (UE 5.8+): the first call turns inspection on and asks for a regenerate, later calls return each execution stack's data (#1309).", "read_pcg_generated_output"),
    ...epicActions,
  },
  {
    ...epicSchema,
    // #1057: every key a spec'd handler declares, generated from its C++
    // registration. A key listed again below is shared with hand-written
    // actions, and tests/unit/handler-specs.test.ts holds the two to one type.
    ...specSchema,
    runId: z.number().optional().describe("wait_for_generation: the runId execute or force_regenerate returned"),
    graceSeconds: z.number().optional().describe("wait_for_generation: with no runId and nothing generating, how long to wait for a deferred start (default 2)"),
  },
);
