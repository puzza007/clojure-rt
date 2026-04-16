dynfib3.clj - timeout (>30s, passes with 60s timeout)
pendulum.clj - needs clojure.core/<= var + extend-protocol (not yet implemented)
protocol.clj - field access in protocol methods segfaults when accessed through specialized fn path
reify.clj - needs RT/count dispatch for deftype/reify (protocol dispatch from C level)
