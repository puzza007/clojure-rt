#include "../CodeGen.h"
#include "runtime/Function.h"
#include <algorithm>
#include <cstddef>
#include <mutex>

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

// Struct offsets -- must match runtime layout (same as FnNode.cpp)
static constexpr size_t kMethodsOffset = offsetof(ClojureFunction, methods);
static constexpr size_t kMethodSize = sizeof(FunctionMethod);
static constexpr size_t kBaselineImplOffset =
    offsetof(FunctionMethod, baselineImplementation);
static constexpr size_t kClosedOversOffset =
    offsetof(FunctionMethod, closedOvers);

TypedValue CodeGen::codegen(const Node &node, const LetfnNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  variableBindingStack.push();
  variableTypesBindingsStack.push();

  // Phase 1: Create all function objects and bind names so mutual references
  // resolve. Method bodies are compiled in Phase 2 when all names are in scope.
  struct FnInfo {
    Value *fnPtr;
    const Node *bindingNode;
    const FnNode *fnSubnode;
    string name;
    uint64_t funId;
  };
  vector<FnInfo> fnInfos;

  // Register all fn ASTs for lazy specialization under a single lock
  static std::mutex astMutex;
  static std::vector<std::unique_ptr<Node>> astStorage;

  for (int i = 0; i < subnode.bindings_size(); i++) {
    auto &bindingNode = subnode.bindings(i);
    auto &binding = bindingNode.subnode().binding();
    auto &initNode = binding.init();
    auto &fnSub = initNode.subnode().fn();

    uint64_t funId = compilerState.nextFnUniqueId.fetch_add(1);

    auto nodeCopy = std::make_unique<Node>(initNode);
    const Node *rawPtr = nodeCopy.get();
    {
      std::lock_guard<std::mutex> lock(astMutex);
      astStorage.push_back(std::move(nodeCopy));
    }
    compilerState.functionAstRegistry.registerObject(
        std::to_string(funId).c_str(), rawPtr);

    // Create the ClojureFunction object
    FunctionType *createFT = FunctionType::get(
        types.ptrTy, {types.i64Ty, types.i64Ty, types.i64Ty, types.i1Ty},
        false);
    Value *fnPtr = invokeManager.invokeRaw(
        "Function_create", createFT,
        {ConstantInt::get(types.i64Ty, fnSub.methods_size()),
         ConstantInt::get(types.i64Ty, funId),
         ConstantInt::get(types.i64Ty, fnSub.maxfixedarity()),
         ConstantInt::get(types.i1Ty, fnSub.once() ? 1 : 0)});

    // Bind immediately so subsequent functions can close over this one
    TypedValue fnTV(ObjectTypeSet(functionType), fnPtr);
    variableBindingStack.set(binding.name(), fnTV);
    variableTypesBindingsStack.set(binding.name(),
                                   ObjectTypeSet(functionType, true));

    // Also bind the fn's local name (localTypeFn self-reference) so that
    // closed-overs referencing the fn by its internal name resolve correctly
    if (fnSub.has_local()) {
      string fnLocalName = fnSub.local().subnode().binding().name();
      variableBindingStack.set(fnLocalName, fnTV);
      variableTypesBindingsStack.set(fnLocalName,
                                     ObjectTypeSet(functionType, true));
    }

    fnInfos.push_back({fnPtr, &bindingNode, &fnSub, binding.name(), funId});
  }

  // Phase 2: Compile method bodies and fill methods for each function.
  // All names are now in scope, so closed-overs resolve correctly.
  for (auto &info : fnInfos) {
    auto &fnSub = *info.fnSubnode;

    // Sort methods: non-variadic desc by arity, then variadic
    struct MethodEntry {
      const FnMethodNode *method;
      int originalIndex;
    };
    vector<MethodEntry> sortedMethods;
    sortedMethods.reserve(fnSub.methods_size());
    for (int i = 0; i < fnSub.methods_size(); i++) {
      sortedMethods.push_back(
          {&fnSub.methods(i).subnode().fnmethod(), i});
    }
    std::sort(sortedMethods.begin(), sortedMethods.end(),
              [](const MethodEntry &a, const MethodEntry &b) {
                if (a.method->isvariadic() && !b.method->isvariadic())
                  return false;
                if (!a.method->isvariadic() && b.method->isvariadic())
                  return true;
                return a.method->fixedarity() > b.method->fixedarity();
              });

    for (size_t pos = 0; pos < sortedMethods.size(); pos++) {
      auto &entry = sortedMethods[pos];
      auto *method = entry.method;

      // Evaluate closed-overs in the current (outer) scope
      vector<Value *> closedOverBoxed;
      closedOverBoxed.reserve(method->closedovers_size());
      for (int j = 0; j < method->closedovers_size(); j++) {
        auto closedOver =
            codegen(method->closedovers(j), ObjectTypeSet::all());
        closedOverBoxed.push_back(valueEncoder.box(closedOver).value);
      }

      // Create LLVM function for this method body
      int numParams = method->fixedarity() + (method->isvariadic() ? 1 : 0);
      string methodFnName =
          "fn_" + to_string(info.funId) + "_m" + to_string(pos);

      vector<Type *> paramTypes(numParams + 1, types.RT_valueTy);
      FunctionType *methodFT =
          FunctionType::get(types.RT_valueTy, paramTypes, false);
      Function *methodFn = Function::Create(
          methodFT, Function::ExternalLinkage, methodFnName, *TheModule);
      methodFn->addFnAttr("frame-pointer", "all");

      // Save outer context
      auto savedIP = Builder.saveIP();
      auto savedMM = memoryManagement.saveState();
      auto savedLexicalBlocks = LexicalBlocks;

      // Set up the method function
      BasicBlock *entryBB =
          BasicBlock::Create(TheContext, "entry", methodFn);
      Builder.SetInsertPoint(entryBB);

      memoryManagement.initFunction(methodFn);

      // Personality function for exception handling
      FunctionType *personalityFnTy = FunctionType::get(types.i32Ty, true);
      auto pFn = TheModule->getOrInsertFunction("__gxx_personality_v0",
                                                 personalityFnTy);
      methodFn->setPersonalityFn(cast<Function>(pFn.getCallee()));

      // Debug info
      auto env = info.bindingNode->env();
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

      // Bind parameters
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
      Value *fnObjArg = &*argIt;

      // Bind closed-overs from the fn object
      if (method->closedovers_size() > 0) {
        TypedValue fnObjBoxed(ObjectTypeSet::dynamicType(), fnObjArg);
        Value *fnRawPtr = valueEncoder.unboxPointer(fnObjBoxed).value;
        Value *baseAddr = Builder.CreatePtrToInt(fnRawPtr, types.i64Ty);

        size_t methodByteOffset = kMethodsOffset + pos * kMethodSize;

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
          const string &coName = method->closedovers(j).subnode().local().name();
          Value *elemAddr = Builder.CreateGEP(
              types.RT_valueTy, closedOversPtr,
              ConstantInt::get(types.i64Ty, j), "co_addr_" + coName);
          Value *coVal =
              Builder.CreateLoad(types.RT_valueTy, elemAddr,
                                 "co_" + coName);
          TypedValue coTV(ObjectTypeSet::dynamicType(), coVal);
          variableBindingStack.set(coName, coTV);
          variableTypesBindingsStack.set(coName, ObjectTypeSet::all());
        }
      }

      // Register recur context
      const string &loopId = method->loopid();
      fnRecurContexts[loopId] = FnRecurContext{methodFn};
      recurContextTypes[loopId] = RecurContextType::Fn;

      // Compile method body
      TypedValue bodyResult = codegen(method->body(), ObjectTypeSet::all());

      if (bodyResult.value != nullptr) {
        Value *boxedResult = valueEncoder.box(bodyResult).value;
        Builder.CreateRet(boxedResult);
      }

      // Cap unterminated BBs
      for (auto &BB : *methodFn) {
        if (!BB.getTerminator()) {
          IRBuilder<> tmpBuilder(&BB);
          tmpBuilder.CreateUnreachable();
        }
      }

      LexicalBlocks.pop_back();
      verifyFunction(*methodFn);

      // Clean up nested context
      variableBindingStack.pop();
      variableTypesBindingsStack.pop();
      recurContextTypes.erase(loopId);
      fnRecurContexts.erase(loopId);

      // Restore outer context
      Builder.restoreIP(savedIP);
      memoryManagement.restoreState(std::move(savedMM));
      LexicalBlocks = savedLexicalBlocks;

      if (!LexicalBlocks.empty()) {
        Builder.SetCurrentDebugLocation(
            DILocation::get(TheContext, env.line(), env.column(),
                            LexicalBlocks.back()));
      }

      // Call Function_fillMethod
      Value *loopIdStr =
          Builder.CreateGlobalStringPtr(method->loopid(), "loopId");

      vector<Value *> fillArgs;
      fillArgs.reserve(7 + method->closedovers_size());
      fillArgs.push_back(info.fnPtr);
      fillArgs.push_back(ConstantInt::get(types.i64Ty, pos));
      fillArgs.push_back(ConstantInt::get(types.i64Ty, entry.originalIndex));
      fillArgs.push_back(
          ConstantInt::get(types.i64Ty, method->fixedarity()));
      fillArgs.push_back(
          ConstantInt::get(types.i1Ty, method->isvariadic()));
      fillArgs.push_back(loopIdStr);
      fillArgs.push_back(ConstantInt::get(
          types.i64Ty, method->closedovers_size()));
      for (auto *co : closedOverBoxed) {
        fillArgs.push_back(co);
      }

      Type *fillParamTypes[] = {types.ptrTy,  types.i64Ty,
                                types.i64Ty,  types.i64Ty,
                                types.i1Ty,   types.ptrTy,
                                types.i64Ty};
      FunctionType *fillFT =
          FunctionType::get(types.voidTy, fillParamTypes, true);
      invokeManager.invokeRaw("Function_fillMethod", fillFT, fillArgs);

      // Store baselineImplementation pointer
      size_t implOffset =
          kMethodsOffset + pos * kMethodSize + kBaselineImplOffset;
      Value *fnPtrAsInt =
          Builder.CreatePtrToInt(info.fnPtr, types.i64Ty);
      Value *implFieldAddr = Builder.CreateAdd(
          fnPtrAsInt, ConstantInt::get(types.i64Ty, implOffset));
      Value *implFieldPtr =
          Builder.CreateIntToPtr(implFieldAddr, types.ptrTy);
      Builder.CreateStore(methodFn, implFieldPtr);
    }
  }

  // Compile the body with all letfn bindings in scope
  auto retVal = codegen(subnode.body(), typeRestrictions);

  variableBindingStack.pop();
  variableTypesBindingsStack.pop();

  return retVal;
}

ObjectTypeSet CodeGen::getType(const Node &node, const LetfnNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  variableTypesBindingsStack.push();

  for (int i = 0; i < subnode.bindings_size(); i++) {
    auto &binding = subnode.bindings(i).subnode().binding();
    auto &fnSub = binding.init().subnode().fn();
    variableTypesBindingsStack.set(binding.name(),
                                   ObjectTypeSet(functionType, true));
    // Also bind the fn's internal local name (localTypeFn self-reference)
    if (fnSub.has_local()) {
      variableTypesBindingsStack.set(
          fnSub.local().subnode().binding().name(),
          ObjectTypeSet(functionType, true));
    }
  }

  auto retVal = getType(subnode.body(), typeRestrictions);
  variableTypesBindingsStack.pop();
  return retVal;
}

} // namespace rt
