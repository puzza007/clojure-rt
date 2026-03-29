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

// Generate the function invoke path (exact match + variadic fallback)
static Value *emitFunctionInvoke(CodeGen &cg, InvokeManager &invokeManager,
                                 ValueEncoder &valueEncoder, LLVMTypes &types,
                                 IRBuilder<> &Builder, LLVMContext &TheContext,
                                 Function *parentFn, Value *fnRawPtr,
                                 Value *fnRTValue, vector<Value *> &boxedArgs,
                                 int N) {
  auto *nullPtr = ConstantPointerNull::get(cast<PointerType>(types.ptrTy));
  FunctionType *ptrI64FT =
      FunctionType::get(types.ptrTy, {types.ptrTy, types.i64Ty}, false);

  // Try non-variadic exact match
  Value *implPtr = invokeManager.invokeRaw(
      "Function_getBaselineImpl", ptrI64FT,
      {fnRawPtr, ConstantInt::get(types.i64Ty, N)});

  Value *isNull =
      Builder.CreateICmpEQ(implPtr, nullPtr, "impl_null");

  BasicBlock *directCallBB =
      BasicBlock::Create(TheContext, "fn_direct", parentFn);
  BasicBlock *tryVariadicBB =
      BasicBlock::Create(TheContext, "fn_try_variadic", parentFn);
  BasicBlock *fnMergeBB =
      BasicBlock::Create(TheContext, "fn_merge", parentFn);

  Builder.CreateCondBr(isNull, tryVariadicBB, directCallBB);

  // Direct call
  Builder.SetInsertPoint(directCallBB);
  vector<Value *> directCallArgs(boxedArgs.begin(), boxedArgs.end());
  directCallArgs.push_back(fnRTValue);
  vector<Type *> directParamTypes(N + 1, types.RT_valueTy);
  FunctionType *directCallFT =
      FunctionType::get(types.RT_valueTy, directParamTypes, false);
  Value *directResult =
      invokeManager.invokeRaw(implPtr, directCallFT, directCallArgs);
  BasicBlock *directExitBB = Builder.GetInsertBlock();
  Builder.CreateBr(fnMergeBB);

  // Variadic fallback
  Builder.SetInsertPoint(tryVariadicBB);

  Value *methodPtr = invokeManager.invokeRaw(
      "Function_findVariadicMethod", ptrI64FT,
      {fnRawPtr, ConstantInt::get(types.i64Ty, N)});

  Value *methodIsNull =
      Builder.CreateICmpEQ(methodPtr, nullPtr, "method_null");
  BasicBlock *arityErrorBB =
      BasicBlock::Create(TheContext, "fn_arity_error", parentFn);
  BasicBlock *varDispatchBB =
      BasicBlock::Create(TheContext, "fn_var_dispatch", parentFn);
  Builder.CreateCondBr(methodIsNull, arityErrorBB, varDispatchBB);

  Builder.SetInsertPoint(arityErrorBB);
  FunctionType *arityExFT =
      FunctionType::get(types.voidTy, {types.i32Ty, types.i32Ty}, false);
  invokeManager.invokeRaw("throwArityException_C", arityExFT,
      {ConstantInt::get(types.i32Ty, -1),
       ConstantInt::get(types.i32Ty, N)});
  Builder.CreateUnreachable();

  Builder.SetInsertPoint(varDispatchBB);

  Value *methodAsInt = Builder.CreatePtrToInt(methodPtr, types.i64Ty);
  Value *implFieldAddr = Builder.CreateAdd(methodAsInt,
      ConstantInt::get(types.i64Ty, kFMBaselineImplOffset));
  Value *vImplPtr = Builder.CreateLoad(types.ptrTy,
      Builder.CreateIntToPtr(implFieldAddr, types.ptrTy), "var_impl");
  Value *arityFieldAddr = Builder.CreateAdd(methodAsInt,
      ConstantInt::get(types.i64Ty, kFMFixedArityOffset));
  Value *fixedArity = Builder.CreateLoad(types.i64Ty,
      Builder.CreateIntToPtr(arityFieldAddr, types.ptrTy), "fixed_arity");

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
  Builder.CreateBr(fnMergeBB);

  // Merge function results
  Builder.SetInsertPoint(fnMergeBB);
  PHINode *fnPhi =
      Builder.CreatePHI(types.RT_valueTy, 2, "fn_result");
  fnPhi->addIncoming(directResult, directExitBB);
  fnPhi->addIncoming(varPhi, varMergeBB);
  return fnPhi;
}

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
  Function *parentFn = Builder.GetInsertBlock()->getParent();

  // If fn type is known at compile time, dispatch directly
  if (fnExpr.type.isDetermined()) {
    objectType fnType = fnExpr.type.determinedType();
    if (fnType == functionType) {
      Value *fnRawPtr = valueEncoder.unboxPointer(
          TypedValue(ObjectTypeSet::dynamicType(), fnRTValue)).value;
      return TypedValue(ObjectTypeSet::dynamicType(),
          emitFunctionInvoke(*this, invokeManager, valueEncoder, types,
                             Builder, TheContext, parentFn, fnRawPtr,
                             fnRTValue, boxedArgs, N));
    }
    if (fnType == persistentVectorType && N == 1) {
      // (vector index) → PersistentVector_nth
      Value *vecPtr = valueEncoder.unboxPointer(
          TypedValue(ObjectTypeSet::dynamicType(), fnRTValue)).value;
      Value *idx = valueEncoder.unboxInt32(
          TypedValue(ObjectTypeSet::dynamicType(), boxedArgs[0])).value;
      Value *idxW = Builder.CreateZExt(idx, types.i64Ty, "nth_idx");
      FunctionType *nthFT = FunctionType::get(
          types.RT_valueTy, {types.ptrTy, types.i64Ty}, false);
      Value *result = invokeManager.invokeRaw(
          "PersistentVector_nth", nthFT, {vecPtr, idxW});
      return TypedValue(ObjectTypeSet::dynamicType(), result);
    }
    if (fnType == persistentArrayMapType && N == 1) {
      // (map key) → PersistentArrayMap_dynamic_get
      auto retType = ObjectTypeSet::dynamicType();
      return invokeManager.invokeRuntime(
          "PersistentArrayMap_dynamic_get", &retType,
          {ObjectTypeSet::dynamicType(), ObjectTypeSet::dynamicType()},
          {TypedValue(ObjectTypeSet::dynamicType(), fnRTValue),
           TypedValue(ObjectTypeSet::dynamicType(), boxedArgs[0])});
    }
    if (fnType == keywordType && N == 1) {
      // (:keyword map) → PersistentArrayMap_dynamic_get(map, keyword)
      auto retType = ObjectTypeSet::dynamicType();
      return invokeManager.invokeRuntime(
          "PersistentArrayMap_dynamic_get", &retType,
          {ObjectTypeSet::dynamicType(), ObjectTypeSet::dynamicType()},
          {TypedValue(ObjectTypeSet::dynamicType(), boxedArgs[0]),
           TypedValue(ObjectTypeSet::dynamicType(), fnRTValue)});
    }
  }

  // Dynamic dispatch: check runtime type
  FunctionType *getTypeFT =
      FunctionType::get(types.i32Ty, {types.RT_valueTy}, false);
  Value *runtimeType =
      invokeManager.invokeRaw("getType", getTypeFT, {fnRTValue});

  BasicBlock *fnBB = BasicBlock::Create(TheContext, "invoke_fn", parentFn);
  BasicBlock *vecBB = (N == 1)
      ? BasicBlock::Create(TheContext, "invoke_vec", parentFn) : nullptr;
  BasicBlock *mapBB = (N == 1)
      ? BasicBlock::Create(TheContext, "invoke_map", parentFn) : nullptr;
  BasicBlock *kwBB = (N == 1)
      ? BasicBlock::Create(TheContext, "invoke_kw", parentFn) : nullptr;
  BasicBlock *errorBB =
      BasicBlock::Create(TheContext, "invoke_error", parentFn);
  BasicBlock *mergeBB =
      BasicBlock::Create(TheContext, "invoke_merge", parentFn);

  SwitchInst *sw = Builder.CreateSwitch(runtimeType, errorBB,
                                        N == 1 ? 4 : 1);
  auto ci = [&](int v) { return cast<ConstantInt>(ConstantInt::get(types.i32Ty, v)); };
  sw->addCase(ci(functionType), fnBB);
  if (N == 1) {
    sw->addCase(ci(persistentVectorType), vecBB);
    sw->addCase(ci(persistentArrayMapType), mapBB);
    sw->addCase(ci(keywordType), kwBB);
  }

  vector<pair<BasicBlock *, Value *>> results;

  // Function path
  Builder.SetInsertPoint(fnBB);
  Value *fnRawPtr = valueEncoder.unboxPointer(
      TypedValue(ObjectTypeSet::dynamicType(), fnRTValue)).value;
  Value *fnResult = emitFunctionInvoke(
      *this, invokeManager, valueEncoder, types, Builder, TheContext,
      parentFn, fnRawPtr, fnRTValue, boxedArgs, N);
  results.push_back({Builder.GetInsertBlock(), fnResult});
  Builder.CreateBr(mergeBB);

  if (N == 1) {
    // Vector path: (vec idx)
    Builder.SetInsertPoint(vecBB);
    Value *vecPtr2 = valueEncoder.unboxPointer(
        TypedValue(ObjectTypeSet::dynamicType(), fnRTValue)).value;
    Value *idx2 = valueEncoder.unboxInt32(
        TypedValue(ObjectTypeSet::dynamicType(), boxedArgs[0])).value;
    Value *idxW2 = Builder.CreateZExt(idx2, types.i64Ty);
    FunctionType *nthFT = FunctionType::get(
        types.RT_valueTy, {types.ptrTy, types.i64Ty}, false);
    Value *vecResult = invokeManager.invokeRaw(
        "PersistentVector_nth", nthFT, {vecPtr2, idxW2});
    results.push_back({Builder.GetInsertBlock(), vecResult});
    Builder.CreateBr(mergeBB);

    // Map path: (map key)
    Builder.SetInsertPoint(mapBB);
    FunctionType *mapGetFT = FunctionType::get(
        types.RT_valueTy, {types.RT_valueTy, types.RT_valueTy}, false);
    Value *mapResult = invokeManager.invokeRaw(
        "PersistentArrayMap_dynamic_get", mapGetFT,
        {fnRTValue, boxedArgs[0]});
    results.push_back({Builder.GetInsertBlock(), mapResult});
    Builder.CreateBr(mergeBB);

    // Keyword path: (:kw map)
    Builder.SetInsertPoint(kwBB);
    Value *kwResult = invokeManager.invokeRaw(
        "PersistentArrayMap_dynamic_get", mapGetFT,
        {boxedArgs[0], fnRTValue});
    results.push_back({Builder.GetInsertBlock(), kwResult});
    Builder.CreateBr(mergeBB);
  }

  // Error path
  Builder.SetInsertPoint(errorBB);
  FunctionType *throwFT =
      FunctionType::get(types.voidTy, {types.ptrTy}, false);
  Value *errMsg = Builder.CreateGlobalStringPtr(
      "Invoke on non-callable type");
  invokeManager.invokeRaw("throwIllegalArgumentException_C", throwFT,
                          {errMsg});
  Builder.CreateUnreachable();

  // Merge
  Builder.SetInsertPoint(mergeBB);
  PHINode *phi = Builder.CreatePHI(types.RT_valueTy, results.size(),
                                   "invoke_result");
  for (auto &[bb, val] : results)
    phi->addIncoming(val, bb);

  return TypedValue(ObjectTypeSet::dynamicType(), phi);
}

ObjectTypeSet CodeGen::getType(const Node &node, const InvokeNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet::all();
}

} // namespace rt
