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

  // atom: (atom init)
  void *atomImpls[] = {nullptr, (void *)core_atom_1};
  createCoreVar(state, "clojure.core/atom", createFixedArityFn(1, atomImpls));

  // deref: (deref ref) — also used by @ reader macro
  void *derefImpls[] = {nullptr, (void *)core_deref_1};
  createCoreVar(state, "clojure.core/deref",
                createFixedArityFn(1, derefImpls));

  // reset!: (reset! atom newval)
  void *resetImpls[] = {nullptr, nullptr, (void *)core_reset_BANG_2};
  createCoreVar(state, "clojure.core/reset!",
                createFixedArityFn(2, resetImpls));

  // swap!: (swap! atom f) or (swap! atom f x)
  void *swapImpls[] = {nullptr, nullptr, (void *)core_swap_BANG_2,
                        (void *)core_swap_BANG_3};
  createCoreVar(state, "clojure.core/swap!",
                createFixedArityFn(3, swapImpls));

  // seq
  void *seqImpls[] = {nullptr, (void *)core_seq_1};
  createCoreVar(state, "clojure.core/seq", createFixedArityFn(1, seqImpls));

  // list
  void *listImpls[] = {(void *)core_list_0, (void *)core_list_1,
                        (void *)core_list_2, (void *)core_list_3};
  createCoreVar(state, "clojure.core/list",
                createFixedArityFn(3, listImpls));

  // inc/dec as vars (needed when passed as values, e.g. (swap! a inc))
  void *incImpls[] = {nullptr, (void *)core_inc_1};
  createCoreVar(state, "clojure.core/inc",
                createFixedArityFn(1, incImpls));

  void *decImpls[] = {nullptr, (void *)core_dec_1};
  createCoreVar(state, "clojure.core/dec",
                createFixedArityFn(1, decImpls));

  // <=
  void *lteImpls[] = {nullptr, nullptr, (void *)core_lte_2};
  createCoreVar(state, "clojure.core/<=",
                createFixedArityFn(2, lteImpls));
}

} // namespace rt
