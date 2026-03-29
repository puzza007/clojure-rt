#include "../../tools/EdnParser.h"
#include "../../tools/RTValueWrapper.h"
#include "../../types/ConstantBool.h"
#include "../../types/ConstantDouble.h"
#include "../../types/ConstantInteger.h"
#include "../CodeGen.h"
#include "bridge/Exceptions.h"
#include "bytecode.pb.h"
#include "codegen/TypedValue.h"
#include <sstream>

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

TypedValue CodeGen::codegen(const Node &node, const InstanceCallNode &subnode,
                             const ObjectTypeSet &typeRestrictions) {
  CleanupChainGuard guard(*this);

  auto instanceVal = codegen(subnode.instance(), ObjectTypeSet::all());
  guard.push(instanceVal);

  std::vector<TypedValue> args;
  for (int i = 0; i < subnode.args_size(); i++) {
    auto t = codegen(subnode.args(i), ObjectTypeSet::all());
    args.push_back(t);
    guard.push(t);
  }

  // For deftype instances with 0 args, route to field access
  if (args.empty() && instanceVal.type.isDetermined() &&
      instanceVal.type.determinedType() == deftypeType) {
    Value *boxedInstance = valueEncoder.box(instanceVal).value;
    Value *instancePtr = valueEncoder.unboxPointer(
        TypedValue(ObjectTypeSet::dynamicType(), boxedInstance)).value;
    Value *fieldNameStr =
        Builder.CreateGlobalStringPtr(subnode.method(), "field_name");
    FunctionType *getFieldFT = FunctionType::get(
        types.RT_valueTy, {types.ptrTy, types.ptrTy}, false);
    Value *fieldVal = invokeManager.invokeRaw(
        "Deftype_getFieldByName", getFieldFT, {instancePtr, fieldNameStr});
    return TypedValue(ObjectTypeSet::dynamicType(), fieldVal);
  }

  // For dynamic instances with 0 args, check if it's a deftype at runtime
  if (args.empty() && !instanceVal.type.isDetermined()) {
    Function *parentFn = Builder.GetInsertBlock()->getParent();
    Value *boxedInstance = valueEncoder.box(instanceVal).value;

    FunctionType *getTypeFT =
        FunctionType::get(types.i32Ty, {types.RT_valueTy}, false);
    Value *runtimeType =
        invokeManager.invokeRaw("getType", getTypeFT, {boxedInstance});
    Value *isDeftype = Builder.CreateICmpEQ(
        runtimeType, ConstantInt::get(types.i32Ty, deftypeType));

    BasicBlock *deftypeBB =
        BasicBlock::Create(TheContext, "ic_deftype", parentFn);
    BasicBlock *generalBB =
        BasicBlock::Create(TheContext, "ic_general", parentFn);
    BasicBlock *mergeBB =
        BasicBlock::Create(TheContext, "ic_merge", parentFn);

    Builder.CreateCondBr(isDeftype, deftypeBB, generalBB);

    // Deftype path: field access
    Builder.SetInsertPoint(deftypeBB);
    Value *instancePtr = valueEncoder.unboxPointer(
        TypedValue(ObjectTypeSet::dynamicType(), boxedInstance)).value;
    Value *fieldNameStr =
        Builder.CreateGlobalStringPtr(subnode.method(), "field_name");
    FunctionType *getFieldFT = FunctionType::get(
        types.RT_valueTy, {types.ptrTy, types.ptrTy}, false);
    Value *fieldVal = invokeManager.invokeRaw(
        "Deftype_getFieldByName", getFieldFT, {instancePtr, fieldNameStr});
    BasicBlock *deftypeExitBB = Builder.GetInsertBlock();
    Builder.CreateBr(mergeBB);

    // General path: delegate to standard instance call dispatch
    Builder.SetInsertPoint(generalBB);
    auto generalResult = invokeManager.generateInstanceCall(
        subnode.method(), instanceVal, args, &guard, &node);
    Value *generalVal = valueEncoder.box(generalResult).value;
    BasicBlock *generalExitBB = Builder.GetInsertBlock();
    Builder.CreateBr(mergeBB);

    Builder.SetInsertPoint(mergeBB);
    PHINode *phi = Builder.CreatePHI(types.RT_valueTy, 2, "ic_result");
    phi->addIncoming(fieldVal, deftypeExitBB);
    phi->addIncoming(generalVal, generalExitBB);
    return TypedValue(ObjectTypeSet::dynamicType(), phi);
  }

  return invokeManager.generateInstanceCall(
      subnode.method(), instanceVal, args, &guard, &node);
}

ObjectTypeSet CodeGen::getType(const Node &node,
                               const InstanceCallNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  auto instanceType = getType(subnode.instance(), ObjectTypeSet::all());
  std::vector<ObjectTypeSet> args;
  for (int i = 0; i < subnode.args_size(); i++) {
    args.push_back(getType(subnode.args(i), ObjectTypeSet::all()).unboxed());
  }

  return this->invokeManager.predictInstanceCallType(subnode.method(), instanceType, args);
}

} // namespace rt
