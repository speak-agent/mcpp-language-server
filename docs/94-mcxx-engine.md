# The mcxx engine: what it answers, and where that is checked

The `mcxx` core engine runs libmc++ (MC++, Sunrisepeak/mcpp-safe) in process: its semantic service
(`mcxx.backend`, Clang 23.1 behind an interface of `msa::` types) and its LSP layer (`mcxx.lsp`). It is
the default engine; `--core-engine clangd` keeps the clangd engine as a fallback. The payload built for
it carries no clangd (`payload --verify` lists `mcxx/resource`, and no `clangd/`).

Every C++ language feature clangd provided is answered by the mcxx engine. The fixture step named for
each runs with `--core-engine mcxx` in CI (a step without `only-engine`, or with `mcxx` in it); most
features have many more such steps across the fixtures.

| Feature | LSP request | libmc++ | A fixture step that checks it on the mcxx engine |
|---|---|---|---|
| Go to definition | `textDocument/definition` | `msa::Workspace::definitions`, the program index | `mcpp-emit` C2, C5 (across a module, into a partition) |
| Go to declaration | `textDocument/declaration` | `msa::Workspace::declarations` | `mcpp-split-gcc` C2-declaration |
| Find references | `textDocument/references` | the program index | `mcpp-emit` C9 |
| Hover | `textDocument/hover` | `msa::Unit::entity_at`, `msa::Entity` | `mcpp-emit` C3 |
| Completion | `textDocument/completion` | `msa::Workspace::complete` | `mcpp-emit` C6, C7 (after an unsaved edit of an imported interface) |
| Diagnostics | `textDocument/publishDiagnostics` | the unit's diagnostics, MC++'s feature gates among them | `mcpp-emit` C1; `module-faults` |
| Semantic highlighting | `textDocument/semanticTokens/full` | `mcxx.lsp` over the unit's occurrences | `inferred` K1-semantic-tokens |
| Document symbols | `textDocument/documentSymbol` | `msa::Unit::symbols` | `inferred` M2-outline |
| Signature help | `textDocument/signatureHelp` | `msa::Workspace::signature_help` | `mcpp-emit` C9-signature |

Also answered, beyond that list: workspace symbols, call hierarchy, document highlight, type
definition and implementation (`mcxx::lsp::Service::capabilities`).

## Declarations before the engine (M2.3)

A definition or declaration request is first the native engine's (`src/engine/native/declarations.cppm`):
MC++'s own front end (`mcxx.frontend`) resolves the name at the position over the file and the
sources of the interfaces it imports -- its module's interface for an implementation unit, what it
imports and what those re-export, each read as the editor has it -- with no BMI and no build. It
answers only what it is sure of: the declarations of what the name names, and for a definition only
one it has read (a class's body, an inline function, a variable). A name `std` declares, a member of a
type it cannot tell, an overload it would have to choose, a function defined in a unit no importer
reads: null, and the mcxx engine answers. So the first jump in a project that has not been built is
answered at once rather than after the modules it imports are built.

A hover and a completion are the native engine's the same way (A2.3.1: their declaration part). A hover
over a name the front end resolves shows its declaration as C++ writes it -- a function's return type,
qualified name and parameters' types (`int cli::run(int)`), a variable's or a member's type, a class's
bases, an alias's type -- and the file it is declared in; a type the front end deduces (`auto`) is not
shown, the engine's hover answers then. After a member access or a qualification being typed (`o.`,
`p->na`, `cli::`) the completion lists the members of the object's class (its bases' too) or of the
scope, from `mcxx::frontend::members_at`; an object whose class the front end cannot tell is the
engine's. An `import` line's completion is the module names, as before. Checked by
`tests/test_native_declarations.cpp`.

Measured by the mcxx workflow's `self-mcpp` job (the mcpp repository, about 170 modules, cold, three
rounds; CI run 36634456673): with the fixture's readiness check first, the first navigation came at
13.77, 12.69 and 12.70 s (median 12.70 s; the clangd path's baseline was 87.8-126 s), the declaration
itself answered 0.02 s after it was asked -- the time was the build description's. The job now runs
`self-mcpp-first-jump`, which asks for the declaration as a user opening the project would: first.
Its first run (CI run 36646514980) answered after 62.0-63.0 s: asked before any model had filled the
native index, the native engine had nothing to answer from, and the request went on to the mcxx
engine, which answered once it had built the modules. A jump or a hover asked before the first model
now waits for it -- the scanned sources' model comes after 2.5 s when the build tool has not
answered, and at most 8 s are waited -- and is then the native engine's like any other.

## A release's gates on the mcxx engine (MS)

What `release-checks.yml` asks of a release, the mcxx workflow asks of the mcxx engine and its
payload, built for release on each platform (`release build from Linux` for linux-x64 too, whose
build job is a dev build):

- **The payload's size (AS.2.2)**: `payload (<platform>, mcxx)` compares the payload with the last
  release that carried clangd (v0.0.6's `payload-<platform>.tar.gz`): unpacked, and as an archive of
  the payload alone; larger fails. Unpacked, v0.0.6's darwin-arm64 payload is 101 MB (clangd 78 MB),
  the mcxx one 77 MB (the server 53 MB, Clang in it); win32-x64 175 MB and 172 MB (the kit, 102 MB, is
  the same in both).
- **Start-up (AS.3.2)**: `release gates` starts the timing fixture five times cold and five times warm;
  the medians of the first navigation must be under 12 s and 5 s (`mcppls-devtools measure summary`).
- **Stability (AS.3.4)**: the same job runs release-checks' stability fixtures three rounds in a row;
  one failure in any round fails it.

They run on a push (a pull request's run is of the same commit).
