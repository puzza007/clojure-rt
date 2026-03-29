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
  FunctionType *rtvalPtrFT =
      FunctionType::get(types.RT_valueTy, {types.ptrTy}, false);
  FunctionType *endCatchFT =
      FunctionType::get(types.voidTy, {}, false);
  FunctionType *rethrowFT =
      FunctionType::get(types.voidTy, {}, false);
  FunctionType *boolPtrPtrFT =
      FunctionType::get(types.i1Ty, {types.ptrTy, types.ptrTy}, false);

  bool hasFinally = subnode.has_finally();

  BasicBlock *catchLpadBB =
      BasicBlock::Create(TheContext, "catch_lpad", parentFn);
  BasicBlock *mergeBB =
      BasicBlock::Create(TheContext, "try_merge", parentFn);

  // Save MM state, set up fresh MM for the try body.
  // After compiling the try body, redirect MM's terminal resume to our
  // catch landing pad. This way all exception-throwing invokes in the
  // try body flow through MM cleanup and then to our catch handler.
  auto savedMM = memoryManagement.saveState();
  memoryManagement.initFunction(parentFn);
  memoryManagement.enterSafetySection(jitEnginePtr);

  // Compile try body inline (same function, can access enclosing variables)
  TypedValue tryResult = codegen(subnode.body(), ObjectTypeSet::all());
  Value *tryResultBoxed = tryResult.value
      ? valueEncoder.box(tryResult).value
      : ConstantInt::get(types.RT_valueTy, 0);

  memoryManagement.leaveSafetySection(jitEnginePtr);

  // Redirect MM's terminal resume to our catch landing pad
  Value *exceptionSlot =
      memoryManagement.redirectTerminalResume(catchLpadBB);

  // Normal exit from try body
  BasicBlock *tryExitBB = Builder.GetInsertBlock();
  Value *normalResult = tryResultBoxed;
  if (hasFinally) {
    codegen(subnode.finally(), ObjectTypeSet::all());
    tryExitBB = Builder.GetInsertBlock();
  }
  Builder.CreateBr(mergeBB);

  // Restore outer MM state
  memoryManagement.restoreState(std::move(savedMM));

  // --- Catch landing pad ---
  // If the MM created exception infrastructure, our catchLpadBB is
  // branched to from the terminal resume. We load the exception from
  // the exception slot. If no exception infrastructure was created
  // (try body can't throw), catchLpadBB is unreachable.
  Builder.SetInsertPoint(catchLpadBB);

  if (!exceptionSlot) {
    // Try body can't throw -- catch is unreachable
    Builder.CreateUnreachable();
    Builder.SetInsertPoint(mergeBB);
    return TypedValue(ObjectTypeSet::dynamicType(), tryResultBoxed);
  }

  // Load the exception from the MM's exception slot
  Value *exVal = Builder.CreateLoad(lpadType, exceptionSlot, "exn_val");
  Value *exnPtr = Builder.CreateExtractValue(exVal, 0, "exn_ptr");

  Value *exnObj = Builder.CreateCall(
      TheModule->getOrInsertFunction("__cxa_begin_catch", voidPtrFT),
      {exnPtr}, "exn_obj");

  // Get exception name and message
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
    Builder.SetInsertPoint(catchBodyBB);

    Builder.CreateCall(
        TheModule->getOrInsertFunction("__cxa_end_catch", endCatchFT), {});

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

  // No match: rethrow
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
  phi->addIncoming(normalResult, tryExitBB);
  for (auto &cr : catchResults)
    phi->addIncoming(cr.result, cr.exitBB);

  return TypedValue(ObjectTypeSet::dynamicType(), phi);
}

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
