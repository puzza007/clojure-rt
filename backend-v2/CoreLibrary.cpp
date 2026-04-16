#include "CoreLibrary.h"
#include "runtime/CoreLibrary.h"
#include "runtime/Function.h"
#include "runtime/Keyword.h"
#include "runtime/String.h"
#include "runtime/Var.h"

namespace rt {

static Var *createCoreVar(ThreadsafeCompilerState &state, const char *name,
                          ClojureFunction *fn) {
  RTValue keyword = Keyword_create(String_createDynamicStr(name));
  Var *var = Var_create(keyword);
  // Set root directly — safe since no old value exists to retire.
  // Mark fn as shared since it will be accessed from JIT threads.
  Object *fnObj = (Object *)fn;
  uword_t count =
      atomic_load_explicit(&fnObj->atomicRefCount, memory_order_relaxed);
  if (!(count & SHARED_BIT)) {
    atomic_store_explicit(&fnObj->atomicRefCount, count | SHARED_BIT,
                          memory_order_release);
  }
  atomic_store_explicit(&var->root, RT_boxPtr(fn), memory_order_release);
  Ptr_retain(var);
  state.varRegistry.registerObject(name, var);
  return var;
}

static ClojureFunction *createFixedArityFn(int maxArity, void **impls) {
  static uint64_t coreId = 900000;
  int methodCount = maxArity + 1;
  ClojureFunction *fn = Function_create(methodCount, coreId++, maxArity, false);
  // Methods sorted: highest arity first (non-variadic desc)
  for (int i = 0; i < methodCount; i++) {
    int arity = maxArity - i;
    Function_fillMethod(fn, i, i, arity, false, (char *)"", 0);
    fn->methods[i].baselineImplementation = impls[arity];
  }
  return fn;
}

void registerCoreLibrary(ThreadsafeCompilerState &state) {
  void *strImpls[] = {(void *)core_str_0, (void *)core_str_1,
                      (void *)core_str_2, (void *)core_str_3};
  createCoreVar(state, "clojure.core/str", createFixedArityFn(3, strImpls));

  void *printlnImpls[] = {(void *)core_println_0, (void *)core_println_1,
                           (void *)core_println_2, (void *)core_println_3};
  createCoreVar(state, "clojure.core/println",
                createFixedArityFn(3, printlnImpls));
}

} // namespace rt
