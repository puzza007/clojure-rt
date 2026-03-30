#include "../CodeGen.h"
#include "runtime/Function.h"
#include <algorithm>
#include <cstddef>
#include <mutex>

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

// Struct offsets computed at C++ compile time -- must match runtime layout
static constexpr size_t kMethodsOffset = offsetof(ClojureFunction, methods);
static constexpr size_t kMethodSize = sizeof(FunctionMethod);
static constexpr size_t kBaselineImplOffset =
    offsetof(FunctionMethod, baselineImplementation);
static constexpr size_t kClosedOversOffset =
    offsetof(FunctionMethod, closedOvers);

TypedValue CodeGen::codegen(const Node &node, const FnNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  uint64_t funId = nextFnUniqueId++;

  // Store a heap copy of the fn AST for lazy specialization.
  // The original node is owned by the caller and may be freed.
  static std::mutex astMutex;
  static std::vector<std::unique_ptr<Node>> astStorage;
  auto nodeCopy = std::make_unique<Node>(node);
  const Node *rawPtr = nodeCopy.get();
  {
    std::lock_guard<std::mutex> lock(astMutex);
    astStorage.push_back(std::move(nodeCopy));
  }
  compilerState.functionAstRegistry.registerObject(
      std::to_string(funId).c_str(), rawPtr);

  // Sort methods: non-variadic desc by arity, then variadic
  struct MethodEntry {
    const Node *methodNode;
    const FnMethodNode *method;
    int originalIndex;
  };
  vector<MethodEntry> sortedMethods;
  for (int i = 0; i < subnode.methods_size(); i++) {
    sortedMethods.push_back(
        {&subnode.methods(i), &subnode.methods(i).subnode().fnmethod(), i});
  }
  std::sort(sortedMethods.begin(), sortedMethods.end(),
            [](const MethodEntry &a, const MethodEntry &b) {
              if (a.method->isvariadic() && !b.method->isvariadic())
                return false;
              if (!a.method->isvariadic() && b.method->isvariadic())
                return true;
              return a.method->fixedarity() > b.method->fixedarity();
            });

  // Create the ClojureFunction object via runtime call
  FunctionType *createFT = FunctionType::get(
      types.ptrTy, {types.i64Ty, types.i64Ty, types.i64Ty, types.i1Ty},
      false);
  Value *fnPtr = invokeManager.invokeRaw(
      "Function_create", createFT,
      {ConstantInt::get(types.i64Ty, subnode.methods_size()),
       ConstantInt::get(types.i64Ty, funId),
       ConstantInt::get(types.i64Ty, subnode.maxfixedarity()),
       ConstantInt::get(types.i1Ty, subnode.once() ? 1 : 0)});

  // For each method, compile body and fill the method slot
  for (size_t pos = 0; pos < sortedMethods.size(); pos++) {
    auto &entry = sortedMethods[pos];
    auto *method = entry.method;

    // --- Evaluate closed-overs in the CURRENT (outer) scope ---
    vector<Value *> closedOverBoxed;
    for (int j = 0; j < method->closedovers_size(); j++) {
      auto closedOver =
          codegen(method->closedovers(j), ObjectTypeSet::all());
      closedOverBoxed.push_back(valueEncoder.box(closedOver).value);
    }

    // --- Create LLVM function for this method body ---
    int numParams = method->fixedarity() + (method->isvariadic() ? 1 : 0);
    string methodFnName =
        "fn_" + to_string(funId) + "_m" + to_string(pos);

    // Params: fixedArity args + fnObj, all as i64 (boxed RTValue)
    vector<Type *> paramTypes(numParams + 1, types.RT_valueTy);
    FunctionType *methodFT =
        FunctionType::get(types.RT_valueTy, paramTypes, false);
    Function *methodFn = Function::Create(methodFT, Function::ExternalLinkage,
                                          methodFnName, *TheModule);
    methodFn->addFnAttr("frame-pointer", "all");

    // --- Save outer context ---
    auto savedIP = Builder.saveIP();
    auto savedMM = memoryManagement.saveState();
    auto savedLexicalBlocks = LexicalBlocks;

    // --- Set up the method function ---
    BasicBlock *entryBB =
        BasicBlock::Create(TheContext, "entry", methodFn);
    Builder.SetInsertPoint(entryBB);

    memoryManagement.initFunction(methodFn);

    // Set up personality function for exception handling
    FunctionType *personalityFnTy = FunctionType::get(types.i32Ty, true);
    auto pFn = TheModule->getOrInsertFunction("__gxx_personality_v0",
                                               personalityFnTy);
    methodFn->setPersonalityFn(cast<Function>(pFn.getCallee()));

    // Create debug info for the method
    auto env = node.env();
    string fileName = env.file();
    string dir = ".";
    if (!fileName.empty()) {
      size_t lastSlash = fileName.find_last_of('/');
      if (lastSlash != string::npos) {
        dir = fileName.substr(0, lastSlash);
        fileName = fileName.substr(lastSlash + 1);
      }
    } else {
      fileName = CU->getFilename().str();
      dir = CU->getDirectory().str();
    }
    DIFile *Unit = DIB->createFile(fileName, dir);
    DISubroutineType *AsmSig =
        DIB->createSubroutineType(DIB->getOrCreateTypeArray({}));
    DISubprogram *SP = DIB->createFunction(
        Unit, methodFnName, methodFnName, Unit, env.line(), AsmSig,
        env.line(), DINode::FlagPrototyped,
        DISubprogram::SPFlagDefinition);
    methodFn->setSubprogram(SP);
    LexicalBlocks.clear();
    LexicalBlocks.push_back(SP);
    Builder.SetCurrentDebugLocation(
        DILocation::get(TheContext, env.line(), env.column(), SP));

    // --- Bind parameters ---
    variableBindingStack.push();
    variableTypesBindingsStack.push();

    auto argIt = methodFn->arg_begin();
    for (int p = 0; p < method->params_size(); p++) {
      auto &paramBinding = method->params(p).subnode().binding();
      TypedValue paramTV(ObjectTypeSet::dynamicType(), &*argIt);
      variableBindingStack.set(paramBinding.name(), paramTV);
      variableTypesBindingsStack.set(paramBinding.name(),
                                     ObjectTypeSet::all());
      argIt++;
    }
    // Last arg is the fn object
    Value *fnObjArg = &*argIt;

    // --- Bind closed-overs from the fn object ---
    if (method->closedovers_size() > 0) {
      // Unbox fn object pointer
      TypedValue fnObjBoxed(ObjectTypeSet::dynamicType(), fnObjArg);
      Value *fnRawPtr = valueEncoder.unboxPointer(fnObjBoxed).value;
      Value *baseAddr = Builder.CreatePtrToInt(fnRawPtr, types.i64Ty);

      size_t methodByteOffset = kMethodsOffset + pos * kMethodSize;

      // Load the closedOvers pointer: methods[pos].closedOvers
      Value *closedOversPtrAddr = Builder.CreateAdd(
          baseAddr,
          ConstantInt::get(types.i64Ty,
                           methodByteOffset + kClosedOversOffset));
      Value *closedOversPtrPtr =
          Builder.CreateIntToPtr(closedOversPtrAddr, types.ptrTy);
      Value *closedOversPtr = Builder.CreateLoad(types.ptrTy,
                                                  closedOversPtrPtr,
                                                  "closedOversPtr");

      for (int j = 0; j < method->closedovers_size(); j++) {
        string coName = method->closedovers(j).subnode().local().name();
        Value *elemAddr = Builder.CreateGEP(
            types.RT_valueTy, closedOversPtr,
            ConstantInt::get(types.i64Ty, j), "co_addr_" + coName);
        Value *coVal =
            Builder.CreateLoad(types.RT_valueTy, elemAddr, "co_" + coName);
        TypedValue coTV(ObjectTypeSet::dynamicType(), coVal);
        variableBindingStack.set(coName, coTV);
        variableTypesBindingsStack.set(coName, ObjectTypeSet::all());
      }
    }

    // --- Register recur context for this method ---
    string loopId = method->loopid();
    fnRecurContexts[loopId] = FnRecurContext{methodFn};
    recurContextTypes[loopId] = RecurContextType::Fn;

    // --- Compile the method body ---
    TypedValue bodyResult = codegen(method->body(), ObjectTypeSet::all());

    // If body didn't terminate via recur, emit return
    if (bodyResult.value != nullptr) {
      Value *boxedResult = valueEncoder.box(bodyResult).value;
      Builder.CreateRet(boxedResult);
    }

    // Cap any unterminated BBs (dead code from recur) before verification
    for (auto &BB : *methodFn) {
      if (!BB.getTerminator()) {
        IRBuilder<> tmpBuilder(&BB);
        tmpBuilder.CreateUnreachable();
      }
    }

    LexicalBlocks.pop_back();
    verifyFunction(*methodFn);

    // --- Clean up nested context ---
    variableBindingStack.pop();
    variableTypesBindingsStack.pop();
    recurContextTypes.erase(loopId);
    fnRecurContexts.erase(loopId);

    // --- Restore outer context ---
    Builder.restoreIP(savedIP);
    memoryManagement.restoreState(std::move(savedMM));
    LexicalBlocks = savedLexicalBlocks;

    // Restore debug location to the outer function's scope
    if (!LexicalBlocks.empty()) {
      Builder.SetCurrentDebugLocation(
          DILocation::get(TheContext, env.line(), env.column(),
                          LexicalBlocks.back()));
    }

    // --- Call Function_fillMethod ---
    Value *loopIdStr =
        Builder.CreateGlobalStringPtr(method->loopid(), "loopId");

    vector<Value *> fillArgs;
    fillArgs.push_back(fnPtr); // self
    fillArgs.push_back(
        ConstantInt::get(types.i64Ty, pos)); // position
    fillArgs.push_back(
        ConstantInt::get(types.i64Ty, entry.originalIndex)); // index
    fillArgs.push_back(
        ConstantInt::get(types.i64Ty, method->fixedarity())); // fixedArity
    fillArgs.push_back(
        ConstantInt::get(types.i1Ty, method->isvariadic())); // isVariadic
    fillArgs.push_back(loopIdStr);                             // loopId
    fillArgs.push_back(ConstantInt::get(
        types.i64Ty, method->closedovers_size())); // closedOversCount
    for (auto *co : closedOverBoxed) {
      fillArgs.push_back(co); // variadic closed-over RTValues
    }

    vector<Type *> fillParamTypes = {types.ptrTy,  types.i64Ty, types.i64Ty,
                                     types.i64Ty,  types.i1Ty,  types.ptrTy,
                                     types.i64Ty};
    FunctionType *fillFT =
        FunctionType::get(types.voidTy, fillParamTypes, true);
    invokeManager.invokeRaw("Function_fillMethod", fillFT, fillArgs);

    // --- Store baselineImplementation pointer ---
    size_t implOffset =
        kMethodsOffset + pos * kMethodSize + kBaselineImplOffset;
    Value *fnPtrAsInt = Builder.CreatePtrToInt(fnPtr, types.i64Ty);
    Value *implFieldAddr = Builder.CreateAdd(
        fnPtrAsInt, ConstantInt::get(types.i64Ty, implOffset));
    Value *implFieldPtr =
        Builder.CreateIntToPtr(implFieldAddr, types.ptrTy);
    Builder.CreateStore(methodFn, implFieldPtr);
  }

  return TypedValue(ObjectTypeSet(functionType), fnPtr);
}

// FnMethodNode cannot be compiled standalone
TypedValue CodeGen::codegen(const Node &node, const FnMethodNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  throwCodeGenerationException(
      "FnMethodNode must be compiled as part of FnNode", node);
  return TypedValue(ObjectTypeSet::all(), nullptr);
}

ObjectTypeSet CodeGen::getType(const Node &node, const FnNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet(functionType, true);
}

ObjectTypeSet CodeGen::getType(const Node &node, const FnMethodNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  throwCodeGenerationException(
      "FnMethodNode must be compiled as part of FnNode", node);
  return ObjectTypeSet::all();
}

} // namespace rt
