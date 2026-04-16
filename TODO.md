# TODO — Prioritized Feature Backlog

## High Impact — Unblocks multiple tests

- [x] Fix `loop.clj` compilation — was `leaveSafetySection` emitted after ret terminator in `compileSpecializedFnMethod`
- [x] Implement `opReify` — anonymous protocol/interface implementation; builds on recent deftype/protocol work; unblocks reify.clj

## Medium Impact — Completes feature areas

- [x] Fix `class-hash-field-access.clj` — was already fixed by `terminateDeadBlocks` in e0396ff
- [x] Implement `opLetfn` — two-phase fn creation for mutual recursion + localTypeFn self-reference support
- [x] Implement `opSet` — PersistentHashSet runtime type + SetNode codegen
- [x] Investigate `dynfib3.clj` timeout — passes with 60s; naive fib(42) is inherently O(2^n)

## Lower Impact — Completeness & polish

- [x] Quote collections — already work via existing VectorNode/MapNode/SetNode codegen
- [ ] Implement `WithMeta` properly — currently ignored (TODO); metadata matters for idiomatic Clojure
- [ ] Implement `opMonitorEnter`/`opMonitorExit` — Java synchronization primitives; low priority unless concurrency is near-term
- [ ] Implement `opPrimInvoke` — optimized primitive interface invocations; performance optimization
