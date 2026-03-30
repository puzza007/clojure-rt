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

// Emit code to encode runtime arg types into the 3x i64 + packed signature
// that specialiseDynamicFn expects
static void emitArgSignature(InvokeManager &invokeManager, LLVMTypes &types,
                             IRBuilder<> &Builder, vector<Value *> &boxedArgs,
                             int N, Value *&sig0, Value *&sig1, Value *&sig2,
                             Value *&packed) {
  FunctionType *getTypeFT =
      FunctionType::get(types.i32Ty, {types.RT_valueTy}, false);

  sig0 = ConstantInt::get(types.i64Ty, 0);
  sig1 = ConstantInt::get(types.i64Ty, 0);
  sig2 = ConstantInt::get(types.i64Ty, 0);
  packed = ConstantInt::get(types.i64Ty, 0);

  Value *sigs[3] = {sig0, sig1, sig2};

  for (int i = 0; i < N && i < 24; i++) {
    // Get runtime type of each arg
    Value *argType = invokeManager.invokeRaw("getType", getTypeFT,
                                              {boxedArgs[i]});
    Value *argTypeWide = Builder.CreateZExt(argType, types.i64Ty);

    int group = i / 8;
    int index = i % 8;

    // Pack type into the right signature word
    Value *shifted = Builder.CreateShl(argTypeWide,
        ConstantInt::get(types.i64Ty, 8 * index));
    sigs[group] = Builder.CreateOr(sigs[group], shifted);

    // All args are boxed in our calling convention (packed bit = 1)
    Value *packedBit = ConstantInt::get(types.i64Ty, 1ULL << i);
    packed = Builder.CreateOr(packed, packedBit);
  }

  sig0 = sigs[0];
  sig1 = sigs[1];
  sig2 = sigs[2];
}

// Emit function invoke via specialiseDynamicFn (type-specialized dispatch)
static Value *emitFunctionInvoke(CodeGen &cg, InvokeManager &invokeManager,
                                 ValueEncoder &valueEncoder, LLVMTypes &types,
                                 IRBuilder<> &Builder, LLVMContext &TheContext,
                                 Function *parentFn, Value *fnRawPtr,
                                 Value *fnRTValue, vector<Value *> &boxedArgs,
                                 int N) {
  auto *nullPtr = ConstantPointerNull::get(cast<PointerType>(types.ptrTy));
  FunctionType *ptrI64FT =
      FunctionType::get(types.ptrTy, {types.ptrTy, types.i64Ty}, false);

  // Encode arg types into signature for specialiseDynamicFn
  Value *sig0, *sig1, *sig2, *packed;
  emitArgSignature(invokeManager, types, Builder, boxedArgs, N,
                   sig0, sig1, sig2, packed);

  // Call specialiseDynamicFn to get a (possibly specialized) function pointer
  Value *jitPtr = ConstantInt::get(types.i64Ty, (uintptr_t)cg.jitEnginePtr);
  jitPtr = Builder.CreateIntToPtr(jitPtr, types.ptrTy);

  FunctionType *specFT = FunctionType::get(
      types.ptrTy,
      {types.ptrTy, types.ptrTy, types.i64Ty,
       types.i64Ty, types.i64Ty, types.i64Ty, types.i64Ty},
      false);

  Value *fptr = invokeManager.invokeRaw(
      "specialiseDynamicFn", specFT,
      {jitPtr, fnRawPtr, ConstantInt::get(types.i64Ty, N),
       sig0, sig1, sig2, packed});

  // Check if specialiseDynamicFn returned NULL (no specialized version available)
  Value *isNull = Builder.CreateICmpEQ(fptr, nullPtr, "spec_null");
  BasicBlock *specBB = BasicBlock::Create(TheContext, "spec_call", parentFn);
  BasicBlock *baselineBB = BasicBlock::Create(TheContext, "baseline_call", parentFn);
  BasicBlock *invokeMergeBB = BasicBlock::Create(TheContext, "invoke_fn_merge", parentFn);
  Builder.CreateCondBr(isNull, baselineBB, specBB);

  // Specialized call path
  Builder.SetInsertPoint(specBB);
  vector<Value *> callArgs(boxedArgs.begin(), boxedArgs.end());
  callArgs.push_back(fnRTValue);
  vector<Type *> callParamTypes(N + 1, types.RT_valueTy);
  FunctionType *callFT =
      FunctionType::get(types.RT_valueTy, callParamTypes, false);
  Value *specResult = invokeManager.invokeRaw(fptr, callFT, callArgs);
  BasicBlock *specExitBB = Builder.GetInsertBlock();
  Builder.CreateBr(invokeMergeBB);

  // Baseline fallback: use Function_getBaselineImpl or variadic
  Builder.SetInsertPoint(baselineBB);
  Value *implPtr = invokeManager.invokeRaw(
      "Function_getBaselineImpl", ptrI64FT,
      {fnRawPtr, ConstantInt::get(types.i64Ty, N)});
  Value *baseIsNull = Builder.CreateICmpEQ(implPtr, nullPtr, "base_null");
  BasicBlock *directBB = BasicBlock::Create(TheContext, "base_direct", parentFn);
  BasicBlock *varBB = BasicBlock::Create(TheContext, "base_variadic", parentFn);
  Builder.CreateCondBr(baseIsNull, varBB, directBB);

  // Direct baseline call
  Builder.SetInsertPoint(directBB);
  vector<Value *> baseCallArgs(boxedArgs.begin(), boxedArgs.end());
  baseCallArgs.push_back(fnRTValue);
  Value *baseResult = invokeManager.invokeRaw(implPtr, callFT, baseCallArgs);
  BasicBlock *directExitBB = Builder.GetInsertBlock();
  Builder.CreateBr(invokeMergeBB);

  // Variadic fallback
  Builder.SetInsertPoint(varBB);
  Value *methodPtr = invokeManager.invokeRaw(
      "Function_findVariadicMethod", ptrI64FT,
      {fnRawPtr, ConstantInt::get(types.i64Ty, N)});
  Value *methodIsNull = Builder.CreateICmpEQ(methodPtr, nullPtr);
  BasicBlock *arityErrBB = BasicBlock::Create(TheContext, "arity_err", parentFn);
  BasicBlock *varDispBB = BasicBlock::Create(TheContext, "var_disp", parentFn);
  Builder.CreateCondBr(methodIsNull, arityErrBB, varDispBB);

  Builder.SetInsertPoint(arityErrBB);
  FunctionType *arityFT = FunctionType::get(types.voidTy, {types.i32Ty, types.i32Ty}, false);
  invokeManager.invokeRaw("throwArityException_C", arityFT,
      {ConstantInt::get(types.i32Ty, -1), ConstantInt::get(types.i32Ty, N)});
  Builder.CreateUnreachable();

  Builder.SetInsertPoint(varDispBB);
  Value *methodAsInt = Builder.CreatePtrToInt(methodPtr, types.i64Ty);
  Value *implFieldAddr = Builder.CreateAdd(methodAsInt,
      ConstantInt::get(types.i64Ty, kFMBaselineImplOffset));
  Value *vImplPtr = Builder.CreateLoad(types.ptrTy,
      Builder.CreateIntToPtr(implFieldAddr, types.ptrTy));
  Value *arityFieldAddr = Builder.CreateAdd(methodAsInt,
      ConstantInt::get(types.i64Ty, kFMFixedArityOffset));
  Value *fixedArity = Builder.CreateLoad(types.i64Ty,
      Builder.CreateIntToPtr(arityFieldAddr, types.ptrTy));

  Value *argsArray = Builder.CreateAlloca(types.RT_valueTy, ConstantInt::get(types.i32Ty, N));
  for (int i = 0; i < N; i++) {
    Builder.CreateStore(boxedArgs[i],
        Builder.CreateGEP(types.RT_valueTy, argsArray, ConstantInt::get(types.i32Ty, i)));
  }
  FunctionType *packFT = FunctionType::get(
      types.RT_valueTy, {types.ptrTy, types.i64Ty, types.i64Ty}, false);
  Value *vecRTValue = invokeManager.invokeRaw("Function_packRestArgs", packFT,
      {argsArray, fixedArity, ConstantInt::get(types.i64Ty, N)});

  BasicBlock *varMergeBB = BasicBlock::Create(TheContext, "var_merge", parentFn);
  BasicBlock *unreachBB = BasicBlock::Create(TheContext, "var_unreach", parentFn);
  SwitchInst *sw = Builder.CreateSwitch(fixedArity, unreachBB, N + 1);
  vector<pair<BasicBlock *, Value *>> varCases;
  for (int fa = 0; fa <= N; fa++) {
    BasicBlock *caseBB = BasicBlock::Create(TheContext, "var_" + to_string(fa), parentFn);
    sw->addCase(cast<ConstantInt>(ConstantInt::get(types.i64Ty, fa)), caseBB);
    Builder.SetInsertPoint(caseBB);
    vector<Value *> varCallArgs;
    for (int j = 0; j < fa; j++) varCallArgs.push_back(boxedArgs[j]);
    varCallArgs.push_back(vecRTValue);
    varCallArgs.push_back(fnRTValue);
    vector<Type *> varPT(fa + 2, types.RT_valueTy);
    Value *varRes = invokeManager.invokeRaw(vImplPtr,
        FunctionType::get(types.RT_valueTy, varPT, false), varCallArgs);
    varCases.push_back({Builder.GetInsertBlock(), varRes});
    Builder.CreateBr(varMergeBB);
  }
  Builder.SetInsertPoint(unreachBB);
  Builder.CreateUnreachable();
  Builder.SetInsertPoint(varMergeBB);
  PHINode *varPhi = Builder.CreatePHI(types.RT_valueTy, varCases.size());
  for (auto &[bb, val] : varCases) varPhi->addIncoming(val, bb);
  Builder.CreateBr(invokeMergeBB);

  // Final merge
  Builder.SetInsertPoint(invokeMergeBB);
  PHINode *phi = Builder.CreatePHI(types.RT_valueTy, 3, "fn_result");
  phi->addIncoming(specResult, specExitBB);
  phi->addIncoming(baseResult, directExitBB);
  phi->addIncoming(varPhi, varMergeBB);
  return phi;
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

  // Static dispatch for known types
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
      Value *vecPtr = valueEncoder.unboxPointer(
          TypedValue(ObjectTypeSet::dynamicType(), fnRTValue)).value;
      FunctionType *nthFT = FunctionType::get(
          types.RT_valueTy, {types.ptrTy, types.RT_valueTy}, false);
      return TypedValue(ObjectTypeSet::dynamicType(),
          invokeManager.invokeRaw("PersistentVector_dynamic_nth", nthFT,
                                  {vecPtr, boxedArgs[0]}));
    }
    if (fnType == persistentArrayMapType && N == 1) {
      auto retType = ObjectTypeSet::dynamicType();
      return invokeManager.invokeRuntime(
          "PersistentArrayMap_dynamic_get", &retType,
          {ObjectTypeSet::dynamicType(), ObjectTypeSet::dynamicType()},
          {TypedValue(ObjectTypeSet::dynamicType(), fnRTValue),
           TypedValue(ObjectTypeSet::dynamicType(), boxedArgs[0])});
    }
    if (fnType == keywordType && N == 1) {
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

  auto ci = [&](int v) { return cast<ConstantInt>(ConstantInt::get(types.i32Ty, v)); };
  SwitchInst *sw = Builder.CreateSwitch(runtimeType, errorBB, N == 1 ? 4 : 1);
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
    Builder.SetInsertPoint(vecBB);
    Value *vecPtr2 = valueEncoder.unboxPointer(
        TypedValue(ObjectTypeSet::dynamicType(), fnRTValue)).value;
    FunctionType *nthFT = FunctionType::get(
        types.RT_valueTy, {types.ptrTy, types.RT_valueTy}, false);
    Value *vecResult = invokeManager.invokeRaw(
        "PersistentVector_dynamic_nth", nthFT, {vecPtr2, boxedArgs[0]});
    results.push_back({Builder.GetInsertBlock(), vecResult});
    Builder.CreateBr(mergeBB);

    Builder.SetInsertPoint(mapBB);
    FunctionType *mapGetFT = FunctionType::get(
        types.RT_valueTy, {types.RT_valueTy, types.RT_valueTy}, false);
    Value *mapResult = invokeManager.invokeRaw(
        "PersistentArrayMap_dynamic_get", mapGetFT, {fnRTValue, boxedArgs[0]});
    results.push_back({Builder.GetInsertBlock(), mapResult});
    Builder.CreateBr(mergeBB);

    Builder.SetInsertPoint(kwBB);
    Value *kwResult = invokeManager.invokeRaw(
        "PersistentArrayMap_dynamic_get", mapGetFT, {boxedArgs[0], fnRTValue});
    results.push_back({Builder.GetInsertBlock(), kwResult});
    Builder.CreateBr(mergeBB);
  }

  Builder.SetInsertPoint(errorBB);
  FunctionType *throwFT =
      FunctionType::get(types.voidTy, {types.ptrTy}, false);
  invokeManager.invokeRaw("throwIllegalArgumentException_C", throwFT,
      {Builder.CreateGlobalStringPtr("Invoke on non-callable type")});
  Builder.CreateUnreachable();

  Builder.SetInsertPoint(mergeBB);
  PHINode *phi = Builder.CreatePHI(types.RT_valueTy, results.size(), "invoke_result");
  for (auto &[bb, val] : results)
    phi->addIncoming(val, bb);

  return TypedValue(ObjectTypeSet::dynamicType(), phi);
}

ObjectTypeSet CodeGen::getType(const Node &node, const InvokeNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet::all();
}

} // namespace rt
