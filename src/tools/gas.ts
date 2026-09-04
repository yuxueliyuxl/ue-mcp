import { z } from "zod";
import { categoryTool, bp, type ToolDef } from "../types.js";

export const gasTool: ToolDef = categoryTool(
  "gas",
  "Gameplay Ability System: abilities, effects, attribute sets, cues.",
  {
    add_asc:             bp("Add AbilitySystemComponent. Params: blueprintPath, componentName?", "add_ability_system_component"),
    create_attribute_set: bp("Create AttributeSet BP. Params: name, packagePath?", "create_attribute_set"),
    add_attribute:       bp("Add attribute to set. Params: attributeSetPath, attributeName, defaultValue?", "add_attribute"),
    create_ability:      bp("Create GameplayAbility BP. Params: name, packagePath?, parentClass?", "create_gameplay_ability"),
    set_ability_tags:    bp("Set tags on ability. Params: abilityPath, ability_tags?, cancel_abilities_with_tag?, activation_required_tags?, activation_blocked_tags?", "set_ability_tags"),
    create_effect:       bp("Create GameplayEffect BP. Params: name, packagePath?, durationPolicy?", "create_gameplay_effect"),
    set_effect_modifier: bp("Add modifier. Params: effectPath, attribute, operation?, magnitude?", "set_effect_modifier"),
    create_cue:          bp("Create GameplayCue. Params: name, packagePath?, cueType?", "create_gameplay_cue"),
    get_info:            bp("Inspect GAS setup. Params: blueprintPath", "get_gas_info"),
    set_asc_defaults:    bp("Wire an AttributeSet onto a Blueprint's ASC component (DefaultStartingData) so attributes exist at runtime. Params: blueprintPath, attributeSet (content path or class name), componentName?, initDataTable? (starting values). Run add_ability_system_component first.", "set_asc_defaults"),
    apply_effect:        bp("Apply a GameplayEffect to a live actor's ASC (agnostic stat/damage stimulus - uses the game's own effect). Params: actorLabel OR actorPath, effectClass (content path or class name), level?, setByCaller? ({tag-or-name: magnitude}), world? (auto|pie|editor, default auto)", "apply_effect"),
    set_attribute:       bp("Set a gameplay attribute's base value on a live actor's ASC (recalculates CurrentValue through the aggregator). Params: actorLabel OR actorPath, attribute (Health | SetName.Health), value, world?", "set_attribute"),
    get_attribute:       bp("Read gameplay attribute base + current values on a live actor's ASC. Omit attribute to list all. Params: actorLabel OR actorPath, attribute?, world?", "get_attribute"),
    init_asc:            bp("Initialize a live actor's ASC (InitAbilityActorInfo) and optionally instantiate an AttributeSet so attributes are live - the runtime setup step for testing a bridge-authored GAS actor. Params: actorLabel OR actorPath, attributeSet? (content path or class name), world?", "init_asc"),
    get_asc_state:       bp("Introspect a live actor's ASC: granted ability specs (class, level, inputID, active, dynamicTags) + owned gameplay tags. Params: actorLabel OR actorPath, world? (auto|pie|editor) (#587)", "get_asc_state", (p) => ({ actorLabel: p.actorLabel, actorPath: p.actorPath, world: p.world })),
    // #956: get_attribute / set_attribute above only see attribute sets the ASC
    // has already registered, and an actor spawned into the editor world has
    // none: the DSO scan that registers them runs in InitializeComponent, which
    // a world that has not begun play never reaches. These two name the set,
    // resolve the instance through the ASC (never the actor's own pointer, which
    // is not proof the ASC knows about it), and register the actor's own
    // subobject when the ASC has not.
    get_live_attribute_value: bp("Read the live value of one FGameplayAttributeData on the attribute set instance actually REGISTERED on an actor's AbilitySystemComponent - equivalent to ASC->GetSet<T>(), not the actor's own subobject pointer. Works in the editor world, where no set is registered yet, by first registering the actor's own sets the way BeginPlay would (set registerOwnerSets=false for a strict read). Returns currentValue and baseValue off the instance plus the aggregator's view of both, and the instance's object path so you can prove which object was read. Params: actorLabel (label or internal name) OR actorPath, attributeSet (content path or class name), attribute (property name, or Set.Property), registerOwnerSets?, world? (#956)", "get_live_attribute_value", (p) => ({ actorLabel: p.actorLabel, actorPath: p.actorPath, attributeSet: p.attributeSet, attribute: p.attribute, registerOwnerSets: p.registerOwnerSets, world: p.world })),
    set_live_attribute_value: bp("Write the live value of one FGameplayAttributeData on the REGISTERED attribute set instance on an actor's AbilitySystemComponent. valueType=\"current\" (default) writes the attribute data in place, which is what staging a mid-combat state needs; valueType=\"base\" writes through the ASC so the aggregator recomputes the current value, which is what a durable change needs. The set's PreAttributeChange may clamp, so the result reports what was actually stored alongside the previous values and the instance's object path. Params: actorLabel (label or internal name) OR actorPath, attributeSet, attribute, value, valueType?, registerOwnerSets?, world? (#956)", "set_live_attribute_value", (p) => ({ actorLabel: p.actorLabel, actorPath: p.actorPath, attributeSet: p.attributeSet, attribute: p.attribute, value: p.value, valueType: p.valueType, registerOwnerSets: p.registerOwnerSets, world: p.world })),
  },
  undefined,
  {
    blueprintPath: z.string().optional(),
    name: z.string().optional(),
    packagePath: z.string().optional(),
    componentName: z.string().optional(),
    attributeSetPath: z.string().optional(),
    attributeName: z.string().optional(),
    defaultValue: z.number().optional(),
    parentClass: z.string().optional(),
    abilityPath: z.string().optional(),
    ability_tags: z.array(z.string()).optional(),
    cancel_abilities_with_tag: z.array(z.string()).optional(),
    block_abilities_with_tag: z.array(z.string()).optional(),
    activation_required_tags: z.array(z.string()).optional(),
    activation_blocked_tags: z.array(z.string()).optional(),
    effectPath: z.string().optional(),
    attribute: z.string().optional(),
    operation: z.string().optional(),
    magnitude: z.number().optional(),
    durationPolicy: z.string().optional(),
    cueType: z.string().optional(),
    // Runtime GAS control (apply_effect / set_attribute / get_attribute)
    actorLabel: z.string().optional().describe("Live actor label/name for runtime GAS actions"),
    effectClass: z.string().optional().describe("apply_effect: GameplayEffect content path or class name"),
    level: z.number().optional().describe("apply_effect: effect level (default 1)"),
    setByCaller: z.record(z.number()).optional().describe("apply_effect: SetByCaller magnitudes keyed by gameplay tag or name"),
    value: z.number().optional().describe("set_attribute: new base value"),
    world: z.string().optional().describe("Runtime world scope: auto (default) | pie | editor"),
    attributeSet: z.string().optional().describe("set_asc_defaults / init_asc: AttributeSet content path or class name"),
    initDataTable: z.string().optional().describe("set_asc_defaults: optional DataTable of starting attribute values"),
    actorPath: z.string().optional().describe("Full actor object path. The unambiguous selector for every runtime GAS action, and it wins over actorLabel when both are given. Editor labels are NOT unique, and a label matching several actors is refused with the candidates rather than resolved at random (#956/#983)"),
    valueType: z.enum(["current", "base"]).optional().describe("set_live_attribute_value: \"current\" (default) writes the attribute data in place; \"base\" writes through the ASC aggregator (#956)"),
    registerOwnerSets: z.boolean().optional().describe("get/set_live_attribute_value: register the actor's own attribute set subobjects on its ASC when it has none, the way BeginPlay would. Default true; false makes the call a strict read of what is already registered (#956)"),
  },
);
