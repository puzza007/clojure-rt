#include "../CodeGen.h"

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

TypedValue CodeGen::codegen(const Node &node, const TryNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  Function *parentFn = Builder.GetInsertBlock()->getParent();
  auto *lpadType =
      StructType::get(TheContext, {types.ptrTy, types.i32Ty});

  auto *typeinfoGV = TheModule->getOrInsertGlobal(
      "_ZTIN2rt17LanguageExceptionE", types.ptrTy);

  FunctionType *voidPtrFT =
      FunctionType::get(types.ptrTy, {types.ptrTy}, false);
  FunctionType *boolPtrPtrFT =
      FunctionType::get(types.i1Ty, {types.ptrTy, types.ptrTy}, false);
  FunctionType *rtvalPtrFT =
      FunctionType::get(types.RT_valueTy, {types.ptrTy}, false);
  FunctionType *endCatchFT =
      FunctionType::get(types.voidTy, {}, false);
  FunctionType *rethrowFT =
      FunctionType::get(types.voidTy, {}, false);

  bool hasFinally = subnode.has_finally();

  // --- Compile try body as a separate LLVM function ---
  // This allows us to `invoke` it and catch exceptions at our landing pad.
  static uint64_t tryCounter = 0;
  string tryFnName = "__try_body_" + to_string(tryCounter++);
  FunctionType *tryFnTy = FunctionType::get(types.RT_valueTy, {}, false);
  Function *tryFn = Function::Create(
      tryFnTy, Function::ExternalLinkage, tryFnName, *TheModule);
  tryFn->addFnAttr("frame-pointer", "all");

  // Set up personality function
  FunctionType *personalityFnTy = FunctionType::get(types.i32Ty, true);
  auto pFn = TheModule->getOrInsertFunction("__gxx_personality_v0",
                                             personalityFnTy);
  tryFn->setPersonalityFn(cast<Function>(pFn.getCallee()));

  // Save outer context and compile try body in the new function
  auto savedIP = Builder.saveIP();
  auto savedMM = memoryManagement.saveState();
  auto savedLexicalBlocks = LexicalBlocks;

  BasicBlock *tryEntryBB =
      BasicBlock::Create(TheContext, "entry", tryFn);
  Builder.SetInsertPoint(tryEntryBB);
  memoryManagement.initFunction(tryFn);

  // Set debug info BEFORE enterSafetySection to avoid wrong-subprogram error
  auto env = node.env();
  string fileName = env.file().empty() ? CU->getFilename().str() : env.file();
  string dir = ".";
  if (!env.file().empty()) {
    size_t lastSlash = fileName.find_last_of('/');
    if (lastSlash != string::npos) {
      dir = fileName.substr(0, lastSlash);
      fileName = fileName.substr(lastSlash + 1);
    }
  } else {
    dir = CU->getDirectory().str();
  }
  DIFile *Unit = DIB->createFile(fileName, dir);
  DISubroutineType *AsmSig =
      DIB->createSubroutineType(DIB->getOrCreateTypeArray({}));
  DISubprogram *SP = DIB->createFunction(
      Unit, tryFnName, tryFnName, Unit, env.line(), AsmSig, env.line(),
      DINode::FlagPrototyped, DISubprogram::SPFlagDefinition);
  tryFn->setSubprogram(SP);
  LexicalBlocks.clear();
  LexicalBlocks.push_back(SP);
  Builder.SetCurrentDebugLocation(
      DILocation::get(TheContext, env.line(), env.column(), SP));

  memoryManagement.enterSafetySection(jitEnginePtr);

  TypedValue tryResult = codegen(subnode.body(), ObjectTypeSet::all());
  Value *tryResultBoxed = tryResult.value
      ? valueEncoder.box(tryResult).value
      : ConstantInt::get(types.RT_valueTy, 0);
  memoryManagement.leaveSafetySection(jitEnginePtr);
  Builder.CreateRet(tryResultBoxed);
  LexicalBlocks.pop_back();
  verifyFunction(*tryFn);

  // Restore outer context
  Builder.restoreIP(savedIP);
  memoryManagement.restoreState(std::move(savedMM));
  LexicalBlocks = savedLexicalBlocks;
  if (!LexicalBlocks.empty()) {
    Builder.SetCurrentDebugLocation(
        DILocation::get(TheContext, env.line(), env.column(),
                        LexicalBlocks.back()));
  }

  // --- Invoke the try body, catching exceptions at our landing pad ---
  BasicBlock *tryNormalBB =
      BasicBlock::Create(TheContext, "try_normal", parentFn);
  BasicBlock *catchLpadBB =
      BasicBlock::Create(TheContext, "catch_lpad", parentFn);

  InvokeInst *tryCall = Builder.CreateInvoke(
      tryFn, tryNormalBB, catchLpadBB, {}, "try_result");

  // --- Normal path (no exception) ---
  Builder.SetInsertPoint(tryNormalBB);
  BasicBlock *mergeBB =
      BasicBlock::Create(TheContext, "try_merge", parentFn);

  Value *normalResult = tryCall;
  BasicBlock *normalExitBB = tryNormalBB;

  if (hasFinally) {
    codegen(subnode.finally(), ObjectTypeSet::all());
    normalExitBB = Builder.GetInsertBlock();
  }
  Builder.CreateBr(mergeBB);

  // --- Catch landing pad ---
  Builder.SetInsertPoint(catchLpadBB);
  LandingPadInst *lp = Builder.CreateLandingPad(lpadType, 1, "catch_lp");
  lp->addClause(typeinfoGV);

  Value *exnPtr = Builder.CreateExtractValue(lp, 0, "exn_ptr");
  Value *exnObj = Builder.CreateCall(
      TheModule->getOrInsertFunction("__cxa_begin_catch", voidPtrFT),
      {exnPtr}, "exn_obj");

  // Get exception name and message for dispatch
  Value *exnName = Builder.CreateCall(
      TheModule->getOrInsertFunction("LanguageException_getName",
          FunctionType::get(types.ptrTy, {types.ptrTy}, false)),
      {exnObj}, "exn_name");
  Value *exnMessage = Builder.CreateCall(
      TheModule->getOrInsertFunction("LanguageException_getMessage", rtvalPtrFT),
      {exnObj}, "exn_msg");

  // --- Dispatch catch clauses ---
  struct CatchResult {
    BasicBlock *exitBB;
    Value *result;
  };
  vector<CatchResult> catchResults;

  BasicBlock *noMatchBB =
      BasicBlock::Create(TheContext, "catch_no_match", parentFn);

  for (int i = 0; i < subnode.catches_size(); i++) {
    auto &catchNode = subnode.catches(i).subnode().catch_();

    // Extract simple class name from the catch clause's class constant
    string className = catchNode.class_().subnode().const_().val();
    if (className.rfind("class ", 0) == 0)
      className = className.substr(6);
    size_t lastDot = className.rfind('.');
    if (lastDot != string::npos)
      className = className.substr(lastDot + 1);

    Value *classNameStr =
        Builder.CreateGlobalStringPtr(className, "catch_class_" + to_string(i));
    Value *isMatch = Builder.CreateCall(
        TheModule->getOrInsertFunction("Exception_isInstance", boolPtrPtrFT),
        {exnName, classNameStr}, "match_" + to_string(i));

    BasicBlock *catchBodyBB = BasicBlock::Create(
        TheContext, "catch_body_" + to_string(i), parentFn);
    BasicBlock *nextCatchBB = (i < subnode.catches_size() - 1)
        ? BasicBlock::Create(TheContext, "try_catch_" + to_string(i + 1), parentFn)
        : noMatchBB;

    Builder.CreateCondBr(isMatch, catchBodyBB, nextCatchBB);

    // --- Catch body ---
    Builder.SetInsertPoint(catchBodyBB);
    Builder.CreateCall(
        TheModule->getOrInsertFunction("__cxa_end_catch", endCatchFT), {});

    // Bind exception to local variable
    variableBindingStack.push();
    variableTypesBindingsStack.push();

    auto &localBinding = catchNode.local().subnode().binding();
    TypedValue exnTV(ObjectTypeSet::dynamicType(), exnMessage);
    variableBindingStack.set(localBinding.name(), exnTV);
    variableTypesBindingsStack.set(localBinding.name(), ObjectTypeSet::all());

    TypedValue catchResult = codegen(catchNode.body(), ObjectTypeSet::all());
    Value *catchResultBoxed = catchResult.value
        ? valueEncoder.box(catchResult).value
        : ConstantInt::get(types.RT_valueTy, 0);

    variableBindingStack.pop();
    variableTypesBindingsStack.pop();

    BasicBlock *catchExitBB = Builder.GetInsertBlock();
    if (hasFinally) {
      codegen(subnode.finally(), ObjectTypeSet::all());
      catchExitBB = Builder.GetInsertBlock();
    }
    Builder.CreateBr(mergeBB);
    catchResults.push_back({catchExitBB, catchResultBoxed});

    if (nextCatchBB != noMatchBB)
      Builder.SetInsertPoint(nextCatchBB);
  }

  // --- No match: rethrow ---
  Builder.SetInsertPoint(noMatchBB);
  Builder.CreateCall(
      TheModule->getOrInsertFunction("__cxa_end_catch", endCatchFT), {});
  Builder.CreateCall(
      TheModule->getOrInsertFunction("__cxa_rethrow", rethrowFT), {});
  Builder.CreateUnreachable();

  // --- Final merge ---
  Builder.SetInsertPoint(mergeBB);
  PHINode *phi = Builder.CreatePHI(types.RT_valueTy,
      1 + catchResults.size(), "try_final");
  phi->addIncoming(normalResult, normalExitBB);
  for (auto &cr : catchResults)
    phi->addIncoming(cr.result, cr.exitBB);

  return TypedValue(ObjectTypeSet::dynamicType(), phi);
}

// CatchNode cannot be compiled standalone
TypedValue CodeGen::codegen(const Node &node, const CatchNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  throwCodeGenerationException(
      "CatchNode must be compiled as part of TryNode", node);
  return TypedValue(ObjectTypeSet::all(), nullptr);
}

ObjectTypeSet CodeGen::getType(const Node &node, const TryNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet::all();
}

ObjectTypeSet CodeGen::getType(const Node &node, const CatchNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet::all();
}

} // namespace rt
