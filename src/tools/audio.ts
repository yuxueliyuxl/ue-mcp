import { z } from "zod";
import { categoryTool, bp, type ToolDef } from "../types.js";
import { Vec3 } from "../schemas.js";

/**
 * Element schemas for the declarative graph-authoring arrays (#936).
 *
 * These were `z.array(z.any())`, which serialises to an array schema with no
 * `items`. VS Code refuses to load any tool carrying one ("tool parameters
 * array type must have items"), which took the whole audio category out of
 * service there. Each shape below mirrors what the C++ handler actually reads,
 * and every object stays open (`.passthrough()`) so a key the handler grew
 * before the schema did still reaches the bridge.
 */

/** metasound_author: a graph input pin. */
const MetaSoundGraphInput = z.object({
  name: z.string().describe("Graph input name"),
  dataType: z.string().describe("MetaSound data type: Float, Int32, Bool, String, Trigger, Audio, Time, ..."),
  default: z.any().optional().describe("Literal default for the input"),
}).passthrough();

/** metasound_author: a graph output pin. */
const MetaSoundGraphOutput = z.object({
  name: z.string().describe("Graph output name"),
  dataType: z.string().describe("MetaSound data type"),
}).passthrough();

/** metasound_author: one node, addressed by a caller-chosen local id. */
const MetaSoundAuthorNode = z.object({
  id: z.string().describe("Local id used by 'connections' to address this node"),
  class: z.string().describe("MetaSound node class name, e.g. 'Sine'"),
  namespace: z.string().optional().describe("Node class namespace (default 'UE')"),
  variant: z.string().optional().describe("Node class variant"),
  majorVersion: z.number().optional().describe("Node class major version (default 1)"),
  inputs: z.record(z.any()).optional().describe("Per-node input defaults, keyed by input name"),
}).passthrough();

/** cue_author: one SoundCue node, addressed by a caller-chosen local id. */
const SoundCueAuthorNode = z.object({
  id: z.string().describe("Local id used by 'connections' to address this node"),
  type: z.string().describe("SoundCue node type: wave_player, mixer, random, modulator, ..."),
  soundWavePath: z.string().optional().describe("wave_player: SoundWave asset to play"),
}).passthrough();

/** metasound_author: 'from'/'to' endpoints, each 'nodeId.pin', 'input.Name', 'output.Name' or 'audioOut.N'. */
const MetaSoundAuthorConnection = z.object({
  from: z.string().describe("Source endpoint: 'nodeId.OutputPin' or 'input.GraphInputName'"),
  to: z.string().describe("Destination endpoint: 'nodeId.InputPin', 'output.GraphOutputName' or 'audioOut.0'"),
}).passthrough();

/** cue_author: parent/child wiring, an omitted or 'root' parent meaning the cue root. */
const SoundCueAuthorConnection = z.object({
  child: z.string().describe("Local id of the child node"),
  parent: z.string().optional().describe("Local id of the parent node; omitted or 'root' wires the cue root"),
  index: z.number().optional().describe("Child slot on the parent (default: append)"),
}).passthrough();

/** create_sound_mix: one per-SoundClass adjustment. */
const SoundClassAdjuster = z.object({
  soundClassPath: z.string().describe("SoundClass asset the adjustment applies to"),
  volumeAdjuster: z.number().optional().describe("Volume multiplier (default 1)"),
  pitchAdjuster: z.number().optional().describe("Pitch multiplier (default 1)"),
  applyToChildren: z.boolean().optional().describe("Apply to child sound classes (default false)"),
}).passthrough();

/**
 * Audio: the full UE5 audio stack, authored end-to-end through the bridge.
 *
 *  - Assets + playback (import, cue/metasound creation, play, ambient).
 *  - MetaSound graph authoring (nodes, inputs/outputs, connections, defaults) via
 *    the MetaSound Builder subsystem - the modern UE5 audio graph.
 *  - SoundCue graph authoring (wave player, mixer, random, modulator, ...).
 *  - Mixing + routing: submixes and submix effect chains, sound classes, sound
 *    mixes, concurrency.
 *  - Spatialization: attenuation assets, and assigning submix/class/attenuation/
 *    concurrency onto sounds.
 *
 * Nothing here creates an empty placeholder: every asset is authorable to a
 * working state through these actions.
 */
export const audioTool: ToolDef = categoryTool(
  "audio",
  "Audio: sound assets, playback, MetaSound + SoundCue graph authoring, submixes/effects, sound classes/mixes, attenuation, concurrency, spatialization.",
  {
    // ── Assets + playback ──────────────────────────────────────────────
    list:              bp("List sound assets (SoundWave, SoundCue, MetaSoundSource) under a directory, paginated. Params: directory? (default /Game), recursive? (default true), maxResults? (default 1000), offset? (default 0). Returns assets, count, total, offset, hasMore, nextOffset (#730).", "list_sound_assets", (p) => ({ directory: p.directory, recursive: p.recursive, maxResults: p.maxResults, offset: p.offset })),
    extract_pcm:       bp("Decode a USoundWave's imported audio to in-memory PCM (no intermediate file, no reliance on the original source path) for semantic sound search / analysis. Returns sampleRate, numChannels, numFrames, durationSeconds, and 16-bit PCM samples base64-encoded (interleaved). Params: soundPath (required), maxSeconds? (cap the decoded window; default full asset), downmixMono? (default false) (#729).", "extract_sound_wave_pcm", (p) => ({ soundPath: p.soundPath ?? p.assetPath, maxSeconds: p.maxSeconds, downmixMono: p.downmixMono })),
    import_audio:      bp("Import a WAV/OGG/FLAC file as a USoundWave. Returns durationSeconds, numChannels, looping. Params: filePath, name?, packagePath? (default /Game/Audio), looping?, replaceExisting? (default true)", "import_audio", (p) => ({ filePath: p.filePath, name: p.name, packagePath: p.packagePath, looping: p.looping, replaceExisting: p.replaceExisting })),
    play_at_location:  bp("Play a sound in the editor world. Params: soundPath, location, volumeMultiplier?, pitchMultiplier?", "play_sound_at_location"),
    spawn_ambient:     bp("Place an AmbientSound actor. Params: soundPath, location, label?", "spawn_ambient_sound"),

    // ── MetaSound ──────────────────────────────────────────────────────
    metasound_author:  bp("PREFERRED: stamp a whole MetaSound graph in ONE call from a declarative spec (avoids dozens of add_node/connect round-trips). Params: name, packagePath?, format? ('mono'|'stereo'), oneShot?, onConflict?, inputs? [{name,dataType,default?}], outputs? [{name,dataType}], nodes? [{id,class,namespace?,variant?,majorVersion?,inputs?:{vertex:value}}], connections? [{from,to}]. Endpoints are 'nodeId:vertex', or special heads 'input:<name>', 'output:<name>', 'audioOut:<channel>'. Each element reports its own ok/error; builds + saves at the end.", "metasound_author", (p) => ({ name: p.name, packagePath: p.packagePath, format: p.format, oneShot: p.oneShot, onConflict: p.onConflict, inputs: p.inputs, outputs: p.outputs, nodes: p.nodes, connections: p.connections })),
    create_metasound:  bp("Create an empty MetaSoundSource and open a builder session for INCREMENTAL authoring (add_node/connect/...). For a whole graph at once prefer metasound_author. Params: name, packagePath? (default /Game/Audio/MetaSounds), format? ('mono'|'stereo'), oneShot?. Returns assetPath.", "create_metasound_source", (p) => ({ name: p.name, packagePath: p.packagePath, format: p.format, oneShot: p.oneShot, onConflict: p.onConflict })),
    metasound_list_node_classes: bp("List common MetaSound node classes to add (name, namespace, variant, notes). Params: filter? (substring).", "metasound_list_node_classes", (p) => ({ filter: p.filter })),
    metasound_get_graph:         bp("Report a MetaSound's builder-session state (active builder, audio outputs, oneShot). Params: assetPath.", "metasound_get_graph", (p) => ({ assetPath: p.assetPath ?? p.metasoundPath })),
    metasound_add_node:          bp("Add a node to a MetaSound graph by registered class name. Returns nodeId (+ input/output counts). Params: assetPath, nodeClassName (e.g. 'Sine'), nodeNamespace? (default 'UE'), nodeVariant? (e.g. 'Audio'), majorVersion? (default 1).", "metasound_add_node", (p) => ({ assetPath: p.assetPath ?? p.metasoundPath, nodeClassName: p.nodeClassName, nodeNamespace: p.nodeNamespace, nodeVariant: p.nodeVariant, majorVersion: p.majorVersion })),
    metasound_add_input:         bp("Add a graph input to a MetaSound. Params: assetPath, name, dataType ('Float'|'Int32'|'Bool'|'String'|'Trigger'|'Audio'|'Time'|...), defaultValue?.", "metasound_add_graph_input", (p) => ({ assetPath: p.assetPath ?? p.metasoundPath, name: p.name, dataType: p.dataType, defaultValue: p.defaultValue })),
    metasound_add_output:        bp("Add a graph output to a MetaSound. Params: assetPath, name, dataType.", "metasound_add_graph_output", (p) => ({ assetPath: p.assetPath ?? p.metasoundPath, name: p.name, dataType: p.dataType })),
    metasound_connect:           bp("Connect one node's output vertex to another node's input vertex. Params: assetPath, fromNodeId, fromOutput (vertex name), toNodeId, toInput (vertex name).", "metasound_connect", (p) => ({ assetPath: p.assetPath ?? p.metasoundPath, fromNodeId: p.fromNodeId, fromOutput: p.fromOutput, toNodeId: p.toNodeId, toInput: p.toInput })),
    metasound_connect_input:     bp("Connect a graph input to a node input vertex. Params: assetPath, graphInput (name), toNodeId, toInput (vertex name).", "metasound_connect_graph_input", (p) => ({ assetPath: p.assetPath ?? p.metasoundPath, graphInput: p.graphInput, toNodeId: p.toNodeId, toInput: p.toInput })),
    metasound_connect_output:    bp("Connect a node output vertex to a graph output. Params: assetPath, fromNodeId, fromOutput (vertex name), graphOutput (name).", "metasound_connect_graph_output", (p) => ({ assetPath: p.assetPath ?? p.metasoundPath, fromNodeId: p.fromNodeId, fromOutput: p.fromOutput, graphOutput: p.graphOutput })),
    metasound_connect_audio_out: bp("Connect a node output vertex to the source's audio output. Params: assetPath, fromNodeId, fromOutput (vertex name, must be Audio type), channel? (0=left/mono, 1=right; default 0).", "metasound_connect_audio_out", (p) => ({ assetPath: p.assetPath ?? p.metasoundPath, fromNodeId: p.fromNodeId, fromOutput: p.fromOutput, channel: p.channel })),
    metasound_set_default:       bp("Set a default value on a node input vertex, or on a graph input. Params: assetPath, value (required), dataType? (Float|Int32|Bool|String hint), then EITHER (nodeId + inputName) OR graphInput.", "metasound_set_input_default", (p) => ({ assetPath: p.assetPath ?? p.metasoundPath, value: p.value, dataType: p.dataType, nodeId: p.nodeId, inputName: p.inputName, graphInput: p.graphInput })),
    metasound_build:             bp("Write the builder document to the MetaSound asset and save. Call after authoring. Params: assetPath.", "metasound_build", (p) => ({ assetPath: p.assetPath ?? p.metasoundPath })),

    // ── SoundCue graph ─────────────────────────────────────────────────
    cue_author:        bp("PREFERRED: create a SoundCue and stamp its whole node tree in ONE call. Params: name, packagePath?, onConflict?, nodes [{id,type,soundWavePath?,properties?}], connections [{parent,child,index?}] (omit parent => root), root? (nodeId). Each element reports ok/error; links + saves at the end.", "soundcue_author", (p) => ({ name: p.name, packagePath: p.packagePath, onConflict: p.onConflict, nodes: p.nodes, connections: p.connections, root: p.root })),
    create_cue:        bp("Create a SoundCue, optionally seeded from a wave. For a whole graph prefer cue_author. Params: name, packagePath?, soundWavePath?.", "create_sound_cue"),
    cue_add_node:      bp("Add a node to a SoundCue graph. Returns nodeId. Params: cuePath, nodeType ('wave_player'|'mixer'|'random'|'modulator'|'attenuation'|'looping'|'concatenator'|'delay'|'switch'), soundWavePath? (wave_player), properties? (node-specific fields).", "soundcue_add_node", (p) => ({ cuePath: p.cuePath ?? p.assetPath, nodeType: p.nodeType, soundWavePath: p.soundWavePath, properties: p.properties })),
    cue_connect:       bp("Connect a SoundCue node as a child of another (or as the cue root). Params: cuePath, parentNodeId (omit for root), childNodeId, childIndex? (default append).", "soundcue_connect", (p) => ({ cuePath: p.cuePath ?? p.assetPath, parentNodeId: p.parentNodeId, childNodeId: p.childNodeId, childIndex: p.childIndex })),
    cue_get_graph:     bp("Read a SoundCue node graph: nodes (id, type, children) and root. Params: cuePath.", "soundcue_get_graph", (p) => ({ cuePath: p.cuePath ?? p.assetPath })),

    // ── Mixing + routing ───────────────────────────────────────────────
    create_submix:     bp("Create a USoundSubmix, optionally parented. Params: name, packagePath? (default /Game/Audio/Submixes), parentPath?, outputVolume?, wetLevel?, dryLevel?.", "create_submix", (p) => ({ name: p.name, packagePath: p.packagePath, parentPath: p.parentPath, outputVolume: p.outputVolume, wetLevel: p.wetLevel, dryLevel: p.dryLevel, onConflict: p.onConflict })),
    set_submix_parent: bp("Reparent a submix (sets ParentSubmix, updating both ends). Params: submixPath, parentPath (empty detaches to root).", "set_submix_parent", (p) => ({ submixPath: p.submixPath, parentPath: p.parentPath })),
    add_submix_effect: bp("Append a submix effect preset to a submix's effect chain (creates the preset asset). Params: submixPath, effectType ('reverb'|'eq'|'dynamics'|'filter'|'delay'), name?, packagePath?, settings? (effect Settings struct as JSON).", "add_submix_effect", (p) => ({ submixPath: p.submixPath, effectType: p.effectType, name: p.name, packagePath: p.packagePath, settings: p.settings })),
    create_sound_class: bp("Create a USoundClass, optionally parented, with properties. Params: name, packagePath? (default /Game/Audio/SoundClasses), parentPath?, properties? (FSoundClassProperties JSON: Volume, Pitch, bIsUISound, ...).", "create_sound_class", (p) => ({ name: p.name, packagePath: p.packagePath, parentPath: p.parentPath, properties: p.properties, onConflict: p.onConflict })),
    create_sound_mix:   bp("Create a USoundMix with sound-class adjusters. Params: name, packagePath? (default /Game/Audio/SoundMixes), adjusters? ([{soundClassPath, volumeAdjuster?, pitchAdjuster?, applyToChildren?}]), fadeInTime?, fadeOutTime?.", "create_sound_mix", (p) => ({ name: p.name, packagePath: p.packagePath, adjusters: p.adjusters, fadeInTime: p.fadeInTime, fadeOutTime: p.fadeOutTime, onConflict: p.onConflict })),
    create_concurrency: bp("Create a USoundConcurrency asset. Params: name, packagePath? (default /Game/Audio/Concurrency), maxCount?, limitToOwner?, resolutionRule? (e.g. 'StopFarthestThenOldest'), volumeScale?.", "create_concurrency", (p) => ({ name: p.name, packagePath: p.packagePath, maxCount: p.maxCount, limitToOwner: p.limitToOwner, resolutionRule: p.resolutionRule, volumeScale: p.volumeScale, onConflict: p.onConflict })),

    // ── Spatialization ─────────────────────────────────────────────────
    create_attenuation: bp("Create a USoundAttenuation asset. Params: name, packagePath? (default /Game/Audio/Attenuation), settings? (FSoundAttenuationSettings JSON), plus shortcuts: falloffDistance?, spatialize?, enableOcclusion?.", "create_attenuation", (p) => ({ name: p.name, packagePath: p.packagePath, settings: p.settings, falloffDistance: p.falloffDistance, spatialize: p.spatialize, enableOcclusion: p.enableOcclusion, onConflict: p.onConflict })),

    // ── Assign routing onto a sound ────────────────────────────────────
    set_sound_submix:      bp("Set a sound's base submix (routing target). Params: soundPath, submixPath (empty detaches).", "set_sound_submix", (p) => ({ soundPath: p.soundPath ?? p.assetPath, submixPath: p.submixPath })),
    add_sound_submix_send: bp("Add a submix send to a sound. Params: soundPath, submixPath, sendLevel? (default 1.0).", "add_sound_submix_send", (p) => ({ soundPath: p.soundPath ?? p.assetPath, submixPath: p.submixPath, sendLevel: p.sendLevel })),
    set_sound_class:       bp("Assign a sound class to a sound. Params: soundPath, soundClassPath.", "set_sound_class", (p) => ({ soundPath: p.soundPath ?? p.assetPath, soundClassPath: p.soundClassPath })),
    set_sound_attenuation: bp("Attach an attenuation asset to a sound. Params: soundPath, attenuationPath (empty clears).", "set_sound_attenuation", (p) => ({ soundPath: p.soundPath ?? p.assetPath, attenuationPath: p.attenuationPath })),
    set_sound_concurrency: bp("Attach a concurrency asset to a sound. Params: soundPath, concurrencyPath (empty clears).", "set_sound_concurrency", (p) => ({ soundPath: p.soundPath ?? p.assetPath, concurrencyPath: p.concurrencyPath })),

    // ── Generic property set (any audio asset) ─────────────────────────
    set_property:      bp("Set any UPROPERTY on an audio asset by (dotted) name, value as JSON. Handles nested structs, arrays, object refs. Params: assetPath, propertyName, value.", "set_audio_property", (p) => ({ assetPath: p.assetPath, propertyName: p.propertyName, value: p.value })),
  },
  undefined,
  {
    // shared / assets
    directory: z.string().optional(), recursive: z.boolean().optional(),
    maxResults: z.number().optional().describe("list: page size (default 1000) (#730)"),
    offset: z.number().optional().describe("list: pagination offset (default 0) (#730)"),
    maxSeconds: z.number().optional().describe("extract_pcm: cap decoded window in seconds (#729)"),
    downmixMono: z.boolean().optional().describe("extract_pcm: average channels to mono (#729)"),
    soundPath: z.string().optional(),
    location: Vec3.optional(),
    volumeMultiplier: z.number().optional(),
    pitchMultiplier: z.number().optional(),
    label: z.string().optional(),
    name: z.string().optional(),
    packagePath: z.string().optional(),
    onConflict: z.string().optional().describe("skip|replace|rename when the asset name exists"),
    soundWavePath: z.string().optional(),
    filePath: z.string().optional().describe("import_audio: path to a WAV/OGG/FLAC file"),
    looping: z.boolean().optional().describe("import_audio: set SoundWave bLooping"),
    replaceExisting: z.boolean().optional().describe("import_audio: replace an existing asset (default true)"),
    assetPath: z.string().optional(),
    propertyName: z.string().optional(),
    value: z.any().optional().describe("JSON value for set_default / set_property"),

    // metasound
    metasoundPath: z.string().optional(),
    format: z.string().optional().describe("create_metasound: 'mono' | 'stereo'"),
    oneShot: z.boolean().optional(),
    filter: z.string().optional(),
    nodeClassName: z.string().optional(),
    nodeNamespace: z.string().optional(),
    nodeVariant: z.string().optional(),
    majorVersion: z.number().optional(),
    dataType: z.string().optional().describe("MetaSound data type: Float, Int32, Bool, String, Trigger, Audio, Time, ..."),
    defaultValue: z.any().optional(),
    fromNodeId: z.string().optional(),
    fromOutput: z.string().optional(),
    toNodeId: z.string().optional(),
    toInput: z.string().optional(),
    graphInput: z.string().optional(),
    graphOutput: z.string().optional(),
    channel: z.number().optional(),
    nodeId: z.string().optional(),
    inputName: z.string().optional(),

    // one-shot declarative graph authoring (metasound_author / cue_author)
    inputs: z.array(MetaSoundGraphInput).optional().describe("metasound_author: [{name,dataType,default?}]"),
    outputs: z.array(MetaSoundGraphOutput).optional().describe("metasound_author: [{name,dataType}]"),
    nodes: z.array(z.union([MetaSoundAuthorNode, SoundCueAuthorNode])).optional().describe("author: node specs. metasound_author: [{id,class,namespace?,variant?,majorVersion?,inputs?}]. cue_author: [{id,type,soundWavePath?,...props}]"),
    connections: z.array(z.union([MetaSoundAuthorConnection, SoundCueAuthorConnection])).optional().describe("author: connection specs. metasound_author: [{from,to}]. cue_author: [{child,parent?,index?}]"),
    root: z.string().optional().describe("cue_author: explicit root nodeId"),

    // soundcue graph
    cuePath: z.string().optional(),
    nodeType: z.string().optional(),
    properties: z.record(z.any()).optional(),
    parentNodeId: z.string().optional(),
    childNodeId: z.string().optional(),
    childIndex: z.number().optional(),

    // mixing / routing
    parentPath: z.string().optional(),
    outputVolume: z.number().optional(),
    wetLevel: z.number().optional(),
    dryLevel: z.number().optional(),
    submixPath: z.string().optional(),
    effectType: z.string().optional(),
    settings: z.record(z.any()).optional(),
    adjusters: z.array(SoundClassAdjuster).optional().describe("create_sound_mix: [{soundClassPath,volumeAdjuster?,pitchAdjuster?,applyToChildren?}]"),
    fadeInTime: z.number().optional(),
    fadeOutTime: z.number().optional(),
    maxCount: z.number().optional(),
    limitToOwner: z.boolean().optional(),
    resolutionRule: z.string().optional(),
    volumeScale: z.number().optional(),

    // spatialization
    falloffDistance: z.number().optional(),
    spatialize: z.boolean().optional(),
    enableOcclusion: z.boolean().optional(),

    // assignment
    soundClassPath: z.string().optional(),
    attenuationPath: z.string().optional(),
    concurrencyPath: z.string().optional(),
    sendLevel: z.number().optional(),
  },
);
