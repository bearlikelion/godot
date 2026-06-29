# GDScript Sandbox: Design Plan

Status: not implemented, planning only.

## Goal

Let SurfsUp load community-authored custom maps that contain real GDScript,
compiled and run for-real (not interpreted data), with no path to `OS`,
`Engine`, file I/O, networking, or anything else that could touch the host
machine or the network. Distribution target is Steam Workshop, so the trust
boundary is "arbitrary file from the internet, opened automatically."

A secondary goal: the SurfsUp SDK is meant to teach people Godot. Map authors
should be writing and learning ordinary GDScript syntax, gameplay logic, and
node patterns, not a stripped-down dialect. The sandbox should remove
*capabilities*, not the language.

## Why GDScript and not Lua

Lua (e.g. via a `godot_luaAPI`-style binding) is the conventional way to get
a hard sandbox in Godot, because the embedding app builds the global table
the script sees from scratch, nothing leaks in by default. We're deliberately
not doing that here, because the SDK's pedagogical goal is for authors to
practice real GDScript, the language they'd use if they later worked on
SurfsUp itself or any other Godot project. That tradeoff means our sandbox
has to be carved out of a language that was never designed to be sandboxed,
which is real engineering risk. Treat this as a known, accepted cost of the
goal, not an oversight.

## Threat model

- Input: a `.gd` (or precompiled `.gdc`) file inside a Workshop-distributed
  map package, loaded automatically when a player selects/joins that map.
- Attacker goal: read/write files outside the sandbox, make network requests,
  exfiltrate data, execute OS commands, crash/DoS the host, or escape to
  touch other players' state in a way the game doesn't intend (relevant once
  this intersects with the MMORPG prototype's multiplayer code).
- Out of scope for v1: protecting against bugs in *our* exposed API surface
  itself (e.g. if we expose a "spawn node" call that has its own logic bug).
  That's an ordinary security review problem, not a sandboxing problem.
- Out of scope entirely: protecting against malicious *binary* GDExtensions
  bundled in a map. Workshop maps must be pure GDScript + scenes/resources,
  no native code. This needs to be enforced at the package/import level
  (reject any `.gdextension`, `.so`, `.dll`, `.dylib` in a map archive),
  not by the script sandbox.

## Why this is hard

GDScript was not designed to be sandboxed. Today, every native class marked
`exposed` in `ClassDB` is visible as a global identifier to every script in
the process, there is one process-wide `GDScriptLanguage` singleton, and it
populates one shared global namespace at `init()` time
(`modules/gdscript/gdscript.cpp`, `GDScriptLanguage::init()`, ~line 2161),
pulling in `ClassDB::get_class_list()` and every `Engine::get_singleton()`
singleton (`OS`, `Input`, `ResourceLoader`, etc.) indiscriminately. There is
no existing allowlist/denylist mechanism anywhere in `core/` or
`modules/gdscript/` (confirmed by grep across both trees). We would be
building the first one.

Two practical implications:

1. We cannot just "turn off a flag." We have to intercept name resolution
   for sandboxed scripts specifically, while leaving the engine's own
   GDScript untouched for everything else (editor, our own game code).
2. Whatever allowlist we build has to be maintained by hand as the engine
   gains new exposed classes. It will rot silently if no one revisits it
   when upgrading Godot versions. This needs a CI check (see "Maintenance"
   below), not just a one-time pass.

## Proposed architecture

### 1. A second, restricted GDScript language context

Introduce a sandboxed variant of script execution that only ever sees an
explicit allowlist of classes/singletons, rather than trying to subtract
from the full list (denylist-by-default is the wrong default for something
shipped to Steam Workshop; an allowlist fails closed when we forget to
update it, a denylist fails open).

Concretely, per the existing engine structure:

- `GDScriptLanguage` is a process-wide singleton
  (`modules/gdscript/gdscript.h`, `static GDScriptLanguage *singleton`)
  that owns one shared `globals` map populated once at `init()`. We do not
  want to fork the whole language implementation; instead, give sandboxed
  scripts their own resolution context.
- Per-script `GDScriptParser`, `GDScriptAnalyzer`, and `GDScriptCompiler`
  instances are already created fresh for every script
  (`GDScript::reload()`, `modules/gdscript/gdscript.cpp`, ~lines 812-849).
  This is the actual hook: tag a `GDScript` resource as sandboxed (e.g. by
  the directory/package it loaded from) and have `reload()` pass that flag
  through to the analyzer.
- In `GDScriptAnalyzer`, all global/class name lookups funnel through
  `reduce_identifier()` / `reduce_identifier_from_base()`
  (`modules/gdscript/gdscript_analyzer.cpp`, ~lines 4049 and 4388) and
  through `class_exists()` (~line 6619), which currently just checks
  `ClassDB::class_exists() && ClassDB::is_class_exposed()`. For a sandboxed
  analyzer instance, this is where we substitute an allowlist check:
  resolution succeeds only if the identifier is in our curated safe set,
  otherwise it's a compile error, not a runtime failure. Compile-time
  rejection is the right failure mode: a map that doesn't compile fails to
  load with a clear error; it never gets a chance to run partially.

### 2. The allowlist itself

Default-deny. Maintain an explicit list of:

- Safe value/utility types: `Vector2`, `Vector3`, `Color`, `Array`,
  `Dictionary`, `String`, math globals, `Curve`, etc.
- Safe node types needed for map authoring: `Node`, `Node3D`, `Area3D`,
  `CollisionShape3D`, `MeshInstance3D`, `AnimationPlayer`, signal-related
  base classes, and whatever subset of SurfsUp's own gameplay node types we
  decide map scripts are allowed to extend or reference.
- Explicitly excluded, always: `OS`, `Engine` (the singleton, not the
  unavoidable language-internal usages), `FileAccess`, `DirAccess`,
  `HTTPRequest`, `HTTPClient`, `TCPServer`, `UDPServer`, `StreamPeerTCP`,
  `WebSocketPeer`, `ResourceLoader`/`ResourceSaver` (arbitrary path load is
  itself an escape hatch), `OS.execute`-adjacent anything, `ClassDB` itself
  (reflection back into the full class list defeats the allowlist),
  `Thread`/`Mutex` (DoS/host stability), `GDExtension`/`GDExtensionManager`.
- A custom, narrow `SandboxAPI` singleton (or autoload-equivalent) that we
  author ourselves: the *only* way a map script reaches back into the game.
  This is the "API into the game" from the original ask, e.g.
  `SandboxAPI.spawn_prop(...)`, `SandboxAPI.get_player_count()`. Every
  method on it is something we wrote and reviewed, so its own attack surface
  is just an ordinary code review problem, not a sandbox-escape problem.

### 3. Enforcement has to be defense in depth, not just the analyzer

The analyzer pass stops *direct* identifier references (`OS.shell_open(...)`
won't compile). It does not stop indirection. Known gaps to close in a real
implementation, not v1 planning hand-waving:

- **`Object.call()` / `callv()` / `Callable` from string method names** can
  invoke methods on an object reference the script already holds without
  the method name ever appearing as a static identifier. If a sandboxed
  script can obtain a reference to *any* object that exposes a dangerous
  method, string-based calls bypass identifier-level filtering entirely.
  Mitigation has to happen at the object-reference boundary too: sandboxed
  scripts should never be handed a reference to anything outside the safe
  set in the first place (don't pass `self.get_tree().root` etc. into
  sandboxed scope; the `SandboxAPI` surface must only ever return safe
  types).
- **`@export` / scene-tree injection**: a map's `.tscn` could reference a
  node script property pointing at something unexpected, or duck-type its
  way to a node we didn't intend it to reach via `get_node()` /
  `find_child()` walking up the tree. The allowlist needs to constrain not
  just identifiers but reachability: sandboxed scripts run under a subtree
  root, and `get_node()`/`get_parent()` either need to be intercepted to
  refuse crossing the subtree boundary, or those node-traversal methods
  themselves go through the same allowlist gate.
- **`load()`/`preload()` of arbitrary resources**: a map could try to
  `load("res://../../something")` or load another script and instantiate
  it to pull in non-sandboxed code. Resource loading from sandboxed scripts
  needs to be restricted to paths inside that map's own package.
- **Infinite loops / memory exhaustion**: not a capability leak, but a
  Steam Workshop map can still hang or OOM the game. Worth a frame-budget
  or instruction-count watchdog on sandboxed script execution, separate
  from the allowlist work.

None of this is exotic, it's the same category of problem as sandboxing any
embedded scripting language, but it means "patch `class_exists()`" is the
starting point of the implementation, not the whole of it.

### 4. Packaging and Steam Workshop integration

- Workshop uploads are a directory tree, not a single `.gd`. The importer
  needs to: reject any non-text/non-resource file types outright (no
  `.gdextension`, `.so`, `.dll`, `.dylib`, no arbitrary `.gdc` bytecode
  that bypassed our compiler, only accept source `.gd` that we compile
  ourselves under sandbox rules), and load all scripts in that package
  through the sandboxed analyzer path, never the normal one.
- `godot-mod-loader` (GodotModding/godot-mod-loader) is not a fit here: it
  works by patching/overriding real game scripts with full engine access,
  which is the opposite of what we want. It's worth keeping in mind only as
  prior art for the *packaging/load-order/metadata* conventions, not for
  its execution model.

## Maintenance

Every Godot engine upgrade can add new `exposed` `ClassDB` classes or new
singletons. The allowlist is opt-in by construction, so a missed update
just means new engine features aren't available to map authors yet (fails
safe), but it should still be checked deliberately on each engine bump: a
small script/test that diffs `ClassDB::get_class_list()` against the
allowlist and flags new exposed classes for a human to triage, rather than
silently doing nothing.

## Suggested implementation order

1. Land the sandboxed-analyzer-context plumbing (tag a `GDScript` as
   sandboxed, thread the flag into a sandboxed `GDScriptAnalyzer` variant)
   with an allowlist that is deliberately tiny (just value types, no nodes)
   and a trivial test map. Prove compile-time rejection works end to end
   before expanding the list.
2. Add the `SandboxAPI` surface with the first one or two real SurfsUp
   hooks (e.g. spawn a prop, read map metadata). Keep it minimal; grow it
   on demand from real map-author requests, not speculatively.
3. Close the indirection gaps (`call()`/`Callable`, node-tree boundary,
   resource loading) before this ever reaches an actual Workshop upload
   path. This step is the one most likely to be underestimated; budget
   real time for it.
4. Packaging/import validation (reject native code, enforce sandboxed
   compile path for everything in a map package).
5. Execution watchdog (loop/memory guard) once the above is solid.

Steps 1-2 are prototype-week-sized. Steps 3-5 are not, they're what
separates a sandbox that demos well from one that's actually safe to put in
front of Steam Workshop's public upload surface.
