dynfib3.clj - timeout (>30s, passes with 60s timeout)
pendulum.clj - needs clojure.core/<= var + extend-protocol (not yet implemented)
protocol.clj - protocol instance call bridge returns instance instead of method result (codegen bug)
reify.clj - needs RT/count dispatch for deftype/reify (protocol dispatch from C level)
