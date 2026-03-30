#include "../jit/JITEngine.h"
#include "../codegen/CodeGen.h"
#include "Exceptions.h"

using namespace rt;
using namespace std;

static void cacheStore(InvokationCache *slot, uint64_t s0, uint64_t s1,
                       uint64_t s2, uint64_t packed, void *fptr) {
  slot->signature[0] = s0;
  slot->signature[1] = s1;
  slot->signature[2] = s2;
  slot->packed = packed;
  atomic_store_explicit(&slot->fptr, fptr, memory_order_release);
}

extern "C" void *specialiseDynamicFn(void *jitPtr, void *funPtr,
                                     uint64_t argCount,
                                     uint64_t argSig0, uint64_t argSig1,
                                     uint64_t argSig2, uint64_t packedArg) {
  auto *jit = static_cast<JITEngine *>(jitPtr);
  auto *fun = static_cast<ClojureFunction *>(funPtr);

  FunctionMethod *method = nullptr;
  int methodIndex = -1;
  for (uword_t i = 0; i < fun->methodCount; i++) {
    FunctionMethod *m = &fun->methods[i];
    if (!m->isVariadic && m->fixedArity == argCount) {
      method = m;
      methodIndex = (int)i;
      break;
    }
    if (m->isVariadic && m->fixedArity <= argCount) {
      method = m;
      methodIndex = (int)i;
      break;
    }
  }

  if (!method)
    throwArityException_C(-1, (int)argCount);

  uint64_t sigs[3] = {argSig0, argSig1, argSig2};
  InvokationCache *emptySlot = nullptr;

  for (int i = 0; i < INVOKATION_CACHE_SIZE; i++) {
    InvokationCache *entry = &method->invocations[i];
    void *cachedFptr = atomic_load_explicit(&entry->fptr, memory_order_acquire);
    if (cachedFptr &&
        entry->signature[0] == argSig0 &&
        entry->signature[1] == argSig1 &&
        entry->signature[2] == argSig2 &&
        entry->packed == packedArg) {
      return cachedFptr;
    }
    if (!cachedFptr && !emptySlot)
      emptySlot = entry;
  }

  // Cache miss: reconstruct arg types from signature
  vector<ObjectTypeSet> argTypes;
  for (uint64_t i = 0; i < argCount; i++) {
    uint64_t group = i / 8;
    uint64_t index = i % 8;
    uint8_t type = (sigs[group] >> (8 * index)) & 0xff;
    bool isBoxed = (packedArg >> i) & 1;
    argTypes.push_back(ObjectTypeSet((objectType)type, isBoxed));
  }

  string fnIdStr = to_string(fun->uniqueId);
  const Node *fnAst = jit->getCompilerState().functionAstRegistry.getCurrent(
      fnIdStr.c_str());

  if (!fnAst)
    return NULL;

  string specializedName = "fn_" + fnIdStr + "_m" + to_string(methodIndex) +
      "_" + ObjectTypeSet::typeStringForArgs(argTypes);

  auto future = jit->compileGeneric(
      [&](CodeGen &cg) -> string {
        return cg.compileSpecializedFnMethod(
            *fnAst, methodIndex, argTypes, fun->uniqueId);
      },
      specializedName, llvm::OptimizationLevel::O2, false, true);

  try {
    auto result = future.get();
    void *fptr = result.address.toPtr<void *>();
    if (emptySlot)
      cacheStore(emptySlot, argSig0, argSig1, argSig2, packedArg, fptr);
    return fptr;
  } catch (...) {
    if (emptySlot)
      cacheStore(emptySlot, argSig0, argSig1, argSig2, packedArg,
                 method->baselineImplementation);
    return method->baselineImplementation;
  }
}
