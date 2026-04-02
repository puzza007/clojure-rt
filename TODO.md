# TODO — Prioritized Feature Backlog

## High Impact — Unblocks multiple tests

- [x] Fix `loop.clj` compilation — was `leaveSafetySection` emitted after ret terminator in `compileSpecializedFnMethod`
- [x] Implement `opReify` — anonymous protocol/interface implementation; builds on recent deftype/protocol work; unblocks reify.clj

## Medium Impact — Completes feature areas

- [ ] Fix `class-hash-field-access.clj` — "Basic Block does not have terminator" codegen bug
- [ ] Implement `opLetfn` — mutual recursion via letfn; test exists (letfn.clj); old backend has reference impl
- [ ] Implement `opSet` — set literals `#{...}`; needs PersistentHashSet runtime type + codegen op
- [ ] Investigate `dynfib3.clj` timeout — fib(42) naive recursion takes ~500M calls; may just need longer timeout or is an optimization opportunity

## Lower Impact — Completeness & polish

- [ ] Quote collections (TODO in QuoteNode.cpp) — currently only handles atoms, not quoted vectors/maps/sets
- [ ] Implement `WithMeta` properly — currently ignored (TODO); metadata matters for idiomatic Clojure
- [ ] Implement `opMonitorEnter`/`opMonitorExit` — Java synchronization primitives; low priority unless concurrency is near-term
- [ ] Implement `opPrimInvoke` — optimized primitive interface invocations; performance optimization
