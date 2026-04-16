dynfib3.clj - timeout (>30s, passes with 60s timeout)
pendulum.clj - needs clojure.core/<= var + extend-protocol (not yet implemented)
protocol.clj - instance call bridge returns nil/this instead of method result; direct (.method obj) also broken
reify.clj - needs RT/count dispatch for deftype/reify (protocol dispatch from C level)
