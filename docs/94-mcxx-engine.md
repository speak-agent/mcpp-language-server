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
