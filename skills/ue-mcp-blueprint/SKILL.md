---
name: ue-mcp-blueprint
description: Use when authoring or modifying Unreal Blueprint assets through ue-mcp. Covers the read-then-write discipline, node/pin wiring, component (SCS) hierarchy, variables, interfaces, compilation, and the difference between pin defaults and linked inputs. Pulls in any time the user asks to create, edit, or inspect a Blueprint.
---

# ue-mcp blueprint authoring

The `blueprint` tool covers reading, authoring, and compiling Blueprints. The default workflow is **read → mutate → compile**, never fire-and-forget.

## Authoring a graph body: prefer the Epic DSL (#711)

When you are authoring the **contents of a graph** (event graph, function body, macro) - a set of nodes plus their wiring - do **not** default to node-by-node `add_node`/`connect_pins`. Reach for Epic's graph DSL first:

1. `blueprint(action="epic_get_graph_dsl_docs")` - read the S-expression grammar once.
2. `blueprint(action="epic_write_graph_dsl", ...)` - author + compile the whole graph body in one call.

The DSL authors and compiles an entire graph in a single round-trip, which is materially faster and more reliable than stitching individual K2Nodes together (one correct pass vs several failed iterations). Use it for graph bodies whenever it is available.

**Availability / fallback.** The `epic_*` actions come from Epic's ToolsetRegistry and require **UE 5.8+ with the Epic toolset plugins enabled**. Check with `epic(action="status")`. When they are unavailable (pre-5.8, or the registry is off), fall back to the native node path below.

**Keep using the native actions for** read/discovery, SCS components, CDO/class defaults, interfaces + event dispatchers, structured `compile`/`validate`, and anything with no Epic equivalent - the native path adds idempotency and rollback the raw tools lack. See the `ue-mcp-epic-routing` skill for the full epic-vs-native decision.

## Discovery before authoring

For any existing Blueprint:

- `blueprint(action="read", assetPath=...)` - structure (parent, components, graphs)
- `blueprint(action="read_graph_summary", assetPath=..., graphName=...)` - lightweight node+edge summary (~10KB) - use this before `read_graph` (which can be 100KB+)
- `blueprint(action="list_graphs", assetPath=...)` - every graph in the BP including event graphs, functions, macros, interface impls
- `blueprint(action="list_variables" | "list_functions" | "list_local_variables")` - the member surface. `list_functions` covers the same graphs as `list_graphs` (own functions, parent overrides, interface impls, event graphs and their entry points, macros, collapsed subgraphs), each tagged with `kind` and `source`; pass `includeInherited: true` to also see overridable functions not implemented yet
- `blueprint(action="get_execution_flow", assetPath=..., entryPoint=...)` - trace exec pins from an entry point
- `blueprint(action="get_dependencies", assetPath=..., reverse=false|true)` - classes/functions/assets this BP uses, or callers if `reverse: true`

## Mutation recipe

1. **Create the skeleton** - `blueprint(action="create", assetPath, parentClass?)`.
2. **Add variables + components** - `add_variable`, `add_component` (pass `parentComponent` for SCS hierarchy).
3. **Build graphs** - `add_node` each K2Node, `set_node_property` for pin defaults, `connect_pins` to wire exec + data.
4. **Compile** - `blueprint(action="compile", assetPath)`. Compilation errors come back in the result; fix them before proceeding.

## Node wiring fundamentals

- `add_node` takes `nodeClass` as a K2Node class short name (`K2Node_CallFunction`, `K2Node_VariableGet`, `K2Node_DynamicCast`, `K2Node_IfThenElse`, etc.) plus `nodeParams` for class-specific fields (`FunctionReference`, `VariableReference`, `TargetType`).
- Pin defaults vs linked values: `set_node_property` writes a literal default onto a pin; `connect_pins` wires a pin to another node's output. A literal default is ignored once a pin is linked.
- `read_node_property` reads either a pin default or a reflected node UPROPERTY - use this to verify the pin was actually set before compiling.
- Graphs with duplicate names (rare but possible after rename) can be disambiguated by passing `graphIndex` alongside `graphName`.

## Components (SCS)

- `add_component` creates a node in the Simple Construction Script. Pass `parentComponent` to put the new component under an existing parent - otherwise it becomes a top-level child of the scene root.
- `set_component_property` writes on the child BP's InheritableComponentHandler override template, not on the parent - the parent stays untouched. This matters when editing inherited components.
- `read_component_properties` dumps every UPROPERTY on the template, including array contents.
- `reparent_component` moves an SCS node to a new parent.

## CDO (class defaults)

- `set_class_default` writes a UPROPERTY on the Blueprint CDO (the class default object). For actor tick settings specifically, use `set_actor_tick_settings` - it handles `bCanEverTick`, `bStartWithTickEnabled`, `TickInterval` in one call.

## Interfaces + event dispatchers

- `create_interface` + `add_interface` - the implement-side.
- `add_event_dispatcher` - fires a multicast delegate from the BP.

## Behavior Tree TASK Blueprints (#886)

A BTTask Blueprint is an ordinary Blueprint whose parent is `UBTTask_BlueprintBase`, so the whole graph is authored with `override_function`, `add_node` and `connect_pins`. There is no separate BT-task authoring action, and none is needed. The two entry points are **override events**, not functions, and the two finishers are ordinary **BlueprintCallable** calls.

Exact sequence for a move task:

```
blueprint(action="create", assetPath="/Game/AI/Tasks/BTT_MoveToTarget",
          parentClass="/Script/AIModule.BTTask_BlueprintBase")

# Entry point. ReceiveExecuteAI is a BlueprintImplementableEvent, so
# override_function places it as an override EVENT in EventGraph and returns
# {"kind": "event", "graphName": "EventGraph", "nodeId": "<guid>"}.
blueprint(action="override_function", assetPath=..., functionName="ReceiveExecuteAI")

# The AI MoveTo node. Its UFUNCTION (UAITask_MoveTo::AIMoveTo) is
# BlueprintInternalUseOnly, so it is NOT a CallFunction: place the K2 node
# class directly.
blueprint(action="add_node", assetPath=..., graphName="EventGraph",
          nodeClass="K2Node_AIMoveTo")

# Finisher. BlueprintCallable on the parent, so a plain CallFunction node.
blueprint(action="add_node", assetPath=..., graphName="EventGraph",
          nodeClass="CallFunction",
          nodeParams={"functionName": "FinishExecute",
                      "className": "/Script/AIModule.BTTask_BlueprintBase"})
blueprint(action="set_node_property", assetPath=..., graphName="EventGraph",
          nodeName="<FinishExecute nodeId>", propertyName="bSuccess", value="true")

blueprint(action="connect_pins", assetPath=..., graphName="EventGraph",
          sourceNode="<ReceiveExecuteAI nodeId>", sourcePin="then",
          targetNode="<AIMoveTo nodeId>", targetPin="execute")
# ... and on to FinishExecute.

# Abort path, same shape.
blueprint(action="override_function", assetPath=..., functionName="ReceiveAbortAI")
blueprint(action="add_node", ..., nodeClass="CallFunction",
          nodeParams={"functionName": "StopMovement",
                      "className": "/Script/Engine.Controller"})
blueprint(action="add_node", ..., nodeClass="CallFunction",
          nodeParams={"functionName": "FinishAbort",
                      "className": "/Script/AIModule.BTTask_BlueprintBase"})

blueprint(action="compile", assetPath=...)
```

Notes that save a round trip:

- `ReceiveExecuteAI` / `ReceiveAbortAI` / `ReceiveTickAI` take `(AAIController* OwnerController, APawn* ControlledPawn)`. The non-AI variants (`ReceiveExecute`, `ReceiveAbort`, `ReceiveTick`) take `(AActor* OwnerActor)` instead. Both sets go through `override_function`.
- `blueprint(action="list_overridable_functions")` on the task Blueprint lists them with `canBeEvent: true`, which is the discovery path if you are unsure of a name.
- A task that overrides `ReceiveExecuteAI` stays active until `FinishExecute` is called, and one that overrides `ReceiveAbortAI` stays aborting until `FinishAbort` is called. Wire both terminators or the task never returns.
- The BehaviorTree ASSET (nodes, decorators, services, the tree structure) is a different surface. This section is only about the task Blueprint's graph.

## Reading a level script Blueprint (#942)

`read`, `list_graphs`, `read_graph`, `read_graph_summary`, `get_execution_flow` and `resolve_graph` accept a World/umap path directly: pass `/Game/Maps/SomeLevel` and the call resolves to that map's level script Blueprint at `PersistentLevel.<MapName>`. The result carries `blueprintPath` and `isLevelScript` so you can see which object answered. A map whose Level Blueprint has never been opened has no level script object yet, and the error says exactly that rather than claiming the Blueprint is missing.

## Auditing call sites across a project (#945)

`blueprint(action="search_call_sites", functionNames=[...])` finds authored call nodes for named functions across a whole directory, including nested and collapsed graphs, in one call. Narrow it with `className` (which also lets the Asset Registry rule most packages out before anything is loaded) and `directory`. Bound the response with `limit`/`offset`, or pass `dumpToFile` to write the full result set to a JSON file the way `read_graph` does. Reach for it instead of enumerating every Blueprint and reading every graph.

Read `stats` before trusting an empty result: `narrowedByRegistry` says whether the dependency filter ran, `blueprintsSkippedByRegistry` says how much it removed unloaded, and `narrowingSkippedReason` says why it did not run. Pass `narrowByRegistry: false` to force a full scan when a result looks too small. Level scripts are not registry assets, so they are only searched when you pass `includeLevelScripts`, which loads a whole map per level.

## Verify before compile

- `blueprint(action="validate")` runs the compiler diagnostics without saving. Cheaper round-trip when iterating.
- `blueprint(action="read_graph_summary")` after mutations confirms the graph shape.

## Verify a default actually landed (#902 / #931)

`blueprint(action="get_variable_default", assetPath, name)` reads the RESOLVED value off the generated class CDO, which is the only value worth comparing after a write plus compile. `list_variables` on its own proves the variable exists, not what it holds.

It also answers whether the value is on DISK. `persisted: false` (with `packageDirty: true`) means the write is live in this editor session and has not reached the package, so it will be gone after a restart. Save the asset and read again before calling a write verified. A readback that only echoed the in-memory value would report success for a write that silently reverts.

Pass `includeValues: true` to `list_variables` for the same resolution across every variable at once, with one persistence verdict for the listing.
