#include "../CodeGen.h"
#include "runtime/Function.h"
#include <cstddef>

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

static constexpr size_t kFMBaselineImplOffset =
    offsetof(FunctionMethod, baselineImplementation);
static constexpr size_t kFMFixedArityOffset =
    offsetof(FunctionMethod, fixedArity);

TypedValue CodeGen::codegen(const Node &node, const InvokeNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  auto fnExpr = codegen(subnode.fn(), ObjectTypeSet::all());

  int N = subnode.args_size();
  vector<Value *> boxedArgs;
  for (int i = 0; i < N; i++) {
    auto arg = codegen(subnode.args(i), ObjectTypeSet::all());
    boxedArgs.push_back(valueEncoder.box(arg).value);
  }

  Value *fnRTValue = valueEncoder.box(fnExpr).value;
  Value *fnRawPtr =
      valueEncoder
          .unboxPointer(TypedValue(ObjectTypeSet::dynamicType(), fnRTValue))
          .value;

  Function *parentFn = Builder.GetInsertBlock()->getParent();
  auto *nullPtr = ConstantPointerNull::get(cast<PointerType>(types.ptrTy));

  // Try non-variadic exact match first
  FunctionType *ptrI64FT =
      FunctionType::get(types.ptrTy, {types.ptrTy, types.i64Ty}, false);
  Value *implPtr = invokeManager.invokeRaw(
      "Function_getBaselineImpl", ptrI64FT,
      {fnRawPtr, ConstantInt::get(types.i64Ty, N)});

  Value *isNull = Builder.CreateICmpEQ(implPtr, nullPtr, "impl_null");

  BasicBlock *directCallBB =
      BasicBlock::Create(TheContext, "invoke_direct", parentFn);
  BasicBlock *tryVariadicBB =
      BasicBlock::Create(TheContext, "invoke_try_variadic", parentFn);
  BasicBlock *mergeBB =
      BasicBlock::Create(TheContext, "invoke_merge", parentFn);

  Builder.CreateCondBr(isNull, tryVariadicBB, directCallBB);

  // --- Non-variadic direct call ---
  Builder.SetInsertPoint(directCallBB);

  vector<Value *> directCallArgs(boxedArgs.begin(), boxedArgs.end());
  directCallArgs.push_back(fnRTValue);
  vector<Type *> directParamTypes(N + 1, types.RT_valueTy);
  FunctionType *directCallFT =
      FunctionType::get(types.RT_valueTy, directParamTypes, false);
  Value *directResult =
      invokeManager.invokeRaw(implPtr, directCallFT, directCallArgs);
  BasicBlock *directExitBB = Builder.GetInsertBlock();
  Builder.CreateBr(mergeBB);

  // --- Variadic fallback ---
  Builder.SetInsertPoint(tryVariadicBB);

  // Find variadic method (returns FunctionMethod* or NULL)
  Value *methodPtr = invokeManager.invokeRaw(
      "Function_findVariadicMethod", ptrI64FT,
      {fnRawPtr, ConstantInt::get(types.i64Ty, N)});

  // No match at all -> arity error
  Value *methodIsNull =
      Builder.CreateICmpEQ(methodPtr, nullPtr, "method_null");
  BasicBlock *arityErrorBB =
      BasicBlock::Create(TheContext, "invoke_arity_error", parentFn);
  BasicBlock *varDispatchBB =
      BasicBlock::Create(TheContext, "invoke_var_dispatch", parentFn);
  Builder.CreateCondBr(methodIsNull, arityErrorBB, varDispatchBB);

  Builder.SetInsertPoint(arityErrorBB);
  FunctionType *arityExFT =
      FunctionType::get(types.voidTy, {types.i32Ty, types.i32Ty}, false);
  invokeManager.invokeRaw("throwArityException_C", arityExFT,
      {ConstantInt::get(types.i32Ty, -1),
       ConstantInt::get(types.i32Ty, N)});
  Builder.CreateUnreachable();

  // --- Variadic dispatch ---
  Builder.SetInsertPoint(varDispatchBB);

  // Load baselineImplementation and fixedArity from the FunctionMethod*
  Value *methodAsInt = Builder.CreatePtrToInt(methodPtr, types.i64Ty);
  Value *implFieldAddr = Builder.CreateAdd(methodAsInt,
      ConstantInt::get(types.i64Ty, kFMBaselineImplOffset));
  Value *vImplPtr = Builder.CreateLoad(types.ptrTy,
      Builder.CreateIntToPtr(implFieldAddr, types.ptrTy), "var_impl");

  Value *arityFieldAddr = Builder.CreateAdd(methodAsInt,
      ConstantInt::get(types.i64Ty, kFMFixedArityOffset));
  Value *fixedArity = Builder.CreateLoad(types.i64Ty,
      Builder.CreateIntToPtr(arityFieldAddr, types.ptrTy), "fixed_arity");

  // Pack rest args via runtime helper (needs args as array)
  Value *argsArray = Builder.CreateAlloca(
      types.RT_valueTy, ConstantInt::get(types.i32Ty, N), "args_array");
  for (int i = 0; i < N; i++) {
    Value *elemPtr = Builder.CreateGEP(types.RT_valueTy, argsArray,
        ConstantInt::get(types.i32Ty, i));
    Builder.CreateStore(boxedArgs[i], elemPtr);
  }

  FunctionType *packFT = FunctionType::get(
      types.RT_valueTy, {types.ptrTy, types.i64Ty, types.i64Ty}, false);
  Value *vecRTValue = invokeManager.invokeRaw(
      "Function_packRestArgs", packFT,
      {argsArray, fixedArity, ConstantInt::get(types.i64Ty, N)});

  // Switch on fixedArity -- each case calls impl with the right param count
  BasicBlock *varMergeBB =
      BasicBlock::Create(TheContext, "var_merge", parentFn);
  BasicBlock *unreachableBB =
      BasicBlock::Create(TheContext, "var_unreachable", parentFn);
  SwitchInst *sw = Builder.CreateSwitch(fixedArity, unreachableBB, N + 1);

  vector<pair<BasicBlock *, Value *>> varCases;
  for (int fa = 0; fa <= N; fa++) {
    BasicBlock *caseBB = BasicBlock::Create(
        TheContext, "var_arity_" + to_string(fa), parentFn);
    sw->addCase(cast<ConstantInt>(ConstantInt::get(types.i64Ty, fa)), caseBB);
    Builder.SetInsertPoint(caseBB);

    // fixed args directly from boxedArgs, then rest vec, then fnObj
    vector<Value *> varCallArgs;
    for (int j = 0; j < fa; j++)
      varCallArgs.push_back(boxedArgs[j]);
    varCallArgs.push_back(vecRTValue);
    varCallArgs.push_back(fnRTValue);

    vector<Type *> varParamTypes(fa + 2, types.RT_valueTy);
    FunctionType *varCallFT =
        FunctionType::get(types.RT_valueTy, varParamTypes, false);
    Value *varResult =
        invokeManager.invokeRaw(vImplPtr, varCallFT, varCallArgs);
    varCases.push_back({Builder.GetInsertBlock(), varResult});
    Builder.CreateBr(varMergeBB);
  }

  Builder.SetInsertPoint(unreachableBB);
  Builder.CreateUnreachable();

  Builder.SetInsertPoint(varMergeBB);
  PHINode *varPhi =
      Builder.CreatePHI(types.RT_valueTy, varCases.size(), "var_result");
  for (auto &[bb, val] : varCases)
    varPhi->addIncoming(val, bb);
  Builder.CreateBr(mergeBB);

  // --- Final merge ---
  Builder.SetInsertPoint(mergeBB);
  PHINode *finalPhi =
      Builder.CreatePHI(types.RT_valueTy, 2, "invoke_result");
  finalPhi->addIncoming(directResult, directExitBB);
  finalPhi->addIncoming(varPhi, varMergeBB);

  return TypedValue(ObjectTypeSet::dynamicType(), finalPhi);
}

ObjectTypeSet CodeGen::getType(const Node &node, const InvokeNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet::all();
}

} // namespace rt
