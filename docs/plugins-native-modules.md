# Native modules

When a plugin needs engine APIs ue-mcp's bridge doesn't already expose, it ships a UE C++ module alongside the npm package. The module compiles into the user's project at install time and registers handlers on the bridge via `UEMCP::RegisterExternalHandler`. This is Shape C from the [overview](plugins.md); most plugins never need it.

`pie-studio` is a real-world example of this shape. Its manifest:

```yaml
actionPrefix: pie                    # used only when injecting into a built-in

nativeModule:
  uePluginName: PIE_Studio           # name of the .uplugin that gets deployed
  minBridgeApi: 2                    # gate against UEMCP_BRIDGE_API_VERSION; 2 = typed contracts
  source: ue/Plugins/PIE_Studio      # path inside your npm tarball
  category: pie                      # surface handlers under a pie(...) tool
  categoryDescription: "PIE record, replay, observe, and input injection"
  specs: handler-specs.json          # recorded C++ contracts (ue-mcp plugin record-specs)
  handlers:
    record_arm:   { description: "Arm the PIE input recorder" }
    replay_arm:   { description: "Arm the PIE input replayer" }
    inject_input:
      description: "Single-frame Enhanced Input inject"
      timeoutSeconds: 5
    # ... more handlers
```

## How handlers become MCP actions

Set `category` and ue-mcp surfaces every handler as an MCP action that dispatches to the bare bridge method your C++ registered (`record_arm` above). No TypeScript task class is involved - the C++ handler *is* the implementation. The category value picks one of two shapes:

- **A new (non-built-in) category** - as in the `pie` example above - is **provisioned as its own top-level tool** the plugin owns. Actions are **not** prefixed (the category is the namespace): `pie(action="record_arm")`. Set `categoryDescription` for the tool's summary. This is the right choice when the handlers form their own domain. Cross-plugin name collisions resolve first-wins, like `provides:`.
- **A built-in category** (e.g. `gameplay`) **injects** the handlers into that existing tool, prefixed with `actionPrefix`: handler `record_arm` becomes `gameplay(action="pie_record_arm")`. Choose this when the handlers belong inside a category that already exists.

Two rules that bite if missed:

- **Declare each handler's parameters in C++, and record them.** See [Typed contracts](#typed-contracts). A handler surfaced without a contract has only the manifest's loose `schema:` (types `string`, `number`, `boolean`, `object`, `array`, with `required:` reported per action but left to the handler to enforce), which cannot express enums, ranges, integers or nested fields. With `specs:` set, a declared handler with no recorded contract is not surfaced at all.
- **`timeoutSeconds`** sets the bridge-call timeout for that action (default 30s). Raise it for long-running handlers.

Omit `category` entirely and handlers are still registered on the bridge but exposed as no MCP action - useful only if another task calls them internally. For an agent-facing plugin you almost always want `category`.

## Typed contracts

Bridge ABI 2 lets a native handler register with the same parameter contract a core ue-mcp handler declares (`MCPHandlerSpec.h`, shipped under the bridge's `Public/`). ue-mcp generates the action's `Params:` clause, its signature and its `describe_action` output from that contract. It checks every call against the contract before anything is sent:

- each parameter's type, including `integer`, `vec3`, `rotator` and `color`;
- `Enum({...})` values on a string or an array of strings, matched case-sensitively;
- `Min`, `Max` and `Range` on a number or integer, or on each element of an array of them;
- `WithFields({...})` on an object or an array of objects, nested to any depth, each field with its own type, enum and range;
- required parameters, aliases, unions, literals, tagged variants (`oneOf`) and choices (`MCPSpec::ExactlyOne`, `MCPSpec::AtLeastOne`);
- any key the contract does not declare, which is refused rather than silently ignored.

```cpp
UEMCP::RegisterExternalHandler(TEXT("stamp_set"), &FStampHandlers::Set, {
    MCPParam::Required(TEXT("actorPath"), EMCPParamType::String, TEXT("The stamp actor")).Alias(TEXT("path")),
    MCPParam::Optional(TEXT("blendMode"), EMCPParamType::String, TEXT("How heights combine"))
        .Enum({ TEXT("Max"), TEXT("Min"), TEXT("Override") }),
    MCPParam::Optional(TEXT("priority"), EMCPParamType::Integer, TEXT("Evaluation order")).Min(0.0),
    MCPParam::Optional(TEXT("quality"), EMCPParamType::Object, TEXT("Per-platform quality")).WithFields({
        MCPParam::RequiredField(TEXT("lod"), EMCPParamType::Integer, TEXT("LOD index")).Range(0.0, 7.0),
    }),
});
```

A contract that fails validation (an enum on a number, a minimum above its maximum, a field declared twice) is logged as an error and dropped, and the call returns `false`. The handler stays registered.

The bridge enforces the contract too. Every call to a handler registered with one is checked before the handler runs, whoever sent it, and refused with `Invalid parameters for <handler>: <reason>`. The bridge then renames declared aliases to their parameter's name, so read only the declared name and do not re-validate what the contract already says. `UEMCP::ContractViolation` (`MCPContract.h`) is the same check, for a handler that wants it for a nested call of its own. For a name-to-scalar map such as graph parameter overrides, declare it `Any` with `.OneOfForms({ EMCPValueForm::ScalarMap })`: each value must be a string, number, boolean or null.

The bridge publishes the contracts in `get_bridge_capabilities.pluginHandlerSpecs`. Record them into the file `nativeModule.specs` names, with an editor running that has your module loaded:

```bash
ue-mcp plugin record-specs --project path/to/Project.uproject
ue-mcp plugin record-specs --project path/to/Project.uproject --check   # CI: fail when the file is stale
```

Ship that file in the tarball (`files:`). The server builds the surface from the recording, so the surface is the same whether or not an editor is connected. `project(get_status)` reports `deployedPlugin.handlerSpecDrift` when the running module registered something different. A call to a drifted handler is checked against the contract the running module registered, since the bridge enforces that one anyway; `describe_action` keeps showing the recording until you re-record it and the client reconnects.

## Layout inside the npm tarball

```
pie-studio/
  ue-mcp.plugin.yml
  dist/                              # tsc output (TypeScript tasks, if any)
  ue/                                # native source ships here
    Plugins/
      PIE_Studio/
        PIE_Studio.uplugin
        Source/
          PIE_Studio/
            PIE_Studio.Build.cs
            Private/
              Handlers/              # handler .cpp files
              PIE/                   # engine subsystem wrappers
              UI/                    # editor UI panels
```

Update `package.json` `files:` so the `ue/` directory ships with the published tarball:

```json
"files": ["dist", "ue", "ue-mcp.plugin.yml", "knowledge", "README.md"]
```

## The native module

Add `UE_MCP_Bridge` to `PrivateDependencyModuleNames` in your `.Build.cs`:

```csharp
public class PIE_Studio : ModuleRules
{
    public PIE_Studio(ReadOnlyTargetRules Target) : base(Target)
    {
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "Json" });
        PrivateDependencyModuleNames.AddRange(new string[] { "UE_MCP_Bridge" });
    }
}
```

Register handlers from `StartupModule`:

```cpp
#include "MCPHandlerRegistration.h"

void FPIE_StudioModule::StartupModule()
{
    UEMCP::RegisterExternalHandler(
        TEXT("inject_input"),
        [](const TSharedPtr<FJsonObject>& Params) -> TSharedPtr<FJsonValue>
        {
            // ... do the work, return a JSON value
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("success"), true);
            return MakeShared<FJsonValueObject>(Result);
        },
        {
            MCPParam::Required(TEXT("action_path"), EMCPParamType::String, TEXT("InputAction asset path")),
            MCPParam::Optional(TEXT("value_x"), EMCPParamType::Number, TEXT("X axis value")).Range(-1.0, 1.0),
        });
}

void FPIE_StudioModule::ShutdownModule()
{
    UEMCP::UnregisterExternalHandler(TEXT("inject_input"));
}
```

The handler's method name (`inject_input`) is the bare bridge method. It's what an auto-surfaced action (`gameplay(action="pie_inject_input")`, via `nativeModule.category`) dispatches to, what a TypeScript task can address through `this.call(...)`, and what the bridge looks up on any dispatch. Register it bare - ue-mcp adds the `actionPrefix` when it surfaces the action.

## Install flow

```bash
ue-mcp plugin install pie-studio
```

The CLI now also:

1. Reads `MCPHandlerRegistration.h` from the deployed bridge and checks that `UEMCP_BRIDGE_API_VERSION >= manifest.nativeModule.minBridgeApi`. Install fails fast if the bridge is too old, with a pointer to `ue-mcp deploy`.
2. Copies `<pkgDir>/<source>` to `<projectDir>/Plugins/<uePluginName>/`.
3. Records every copied file in `<projectDir>/.ue-mcp/native-modules.json` so `ue-mcp plugin uninstall` can clean up without nuking user edits.
4. Prints `REBUILD REQUIRED` - the user must build the UE project before launching the editor so the new module compiles in.

## Bridge ABI versioning

`UEMCP_BRIDGE_API_VERSION` is the C++ ABI contract every native plugin compiles against. Bumps are reserved for changes to the `FExternalHandlerFn` signature or the registration contract. Version 2 added typed contracts; a module that registers with one needs `minBridgeApi: 2`. A plugin declaring `minBridgeApi: N` refuses to load against a bridge whose version is below N. Inspect the deployed bridge's version with:

```text
project(action="get_status")
```

The response includes `bridgeApiVersion` when a bridge is deployed.
