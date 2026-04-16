dynfib3.clj - passes with 60s timeout (naive fib(42) is inherently slow)
pendulum.clj - needs clojure.core/<= var + extend-protocol (not yet implemented)
protocol.clj - (:field this) keyword invoke on deftypes not supported (use direct field access instead)
reify.clj - needs RT/count dispatch for deftype/reify (protocol dispatch from C level)
