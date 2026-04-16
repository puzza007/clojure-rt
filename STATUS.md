dynfib3.clj - passes with 60s timeout (naive fib(42) is inherently slow)
pendulum.clj - needs extend-protocol (not yet implemented)
protocol.clj - PASSES
reify.clj - needs RT/count dispatch for deftype/reify (protocol dispatch from C level)

Known limitations:
- user-defined variadic fns `(defn f [& args] ...)` crash in Release mode (works in Debug).
  Attempted fix in CodeGen.cpp but hits LLVM O3 optimizer issue during JIT compilation.
  Workaround: register fixed-arity methods (up to arg 3) in CoreLibrary.cpp.
