dynfib3.clj - passes with 60s timeout (naive fib(42) is inherently slow)
pendulum.clj - needs clojure.core/<= var + extend-protocol (not yet implemented)
protocol.clj - multi-arity protocol method dispatch (2-arg loud-sound) falls back to 1-arg version
reify.clj - needs RT/count dispatch for deftype/reify (protocol dispatch from C level)
