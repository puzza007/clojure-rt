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

TypedValue CodeGen::codegen(const Node &node, const HostInteropNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  CleanupChainGuard guard(*this);

  auto targetVal = codegen(subnode.target(), ObjectTypeSet::all());
  guard.push(targetVal);

  string memberName = subnode.morf();

  // Deftype field access: (.fieldName instance) compiles as host-interop
  // when the analyzer can't resolve the member statically
  bool mayBeDeftype = !targetVal.type.isDetermined() ||
                      targetVal.type.determinedType() == deftypeType;
  if (mayBeDeftype) {
    Value *boxedTarget = valueEncoder.box(targetVal).value;

    if (targetVal.type.isDetermined() &&
        targetVal.type.determinedType() == deftypeType) {
      // Known deftype: direct field access
      Value *targetPtr = valueEncoder.unboxPointer(
          TypedValue(ObjectTypeSet::dynamicType(), boxedTarget)).value;
      Value *fieldNameStr =
          Builder.CreateGlobalStringPtr(memberName, "field_name");
      FunctionType *getFieldFT = FunctionType::get(
          types.RT_valueTy, {types.ptrTy, types.ptrTy}, false);
      return TypedValue(ObjectTypeSet::dynamicType(),
          invokeManager.invokeRaw("Deftype_getFieldByName", getFieldFT,
              {targetPtr, fieldNameStr}));
    }

    // Dynamic type: check at runtime
    Function *parentFn = Builder.GetInsertBlock()->getParent();
    FunctionType *getTypeFT =
        FunctionType::get(types.i32Ty, {types.RT_valueTy}, false);
    Value *runtimeType =
        invokeManager.invokeRaw("getType", getTypeFT, {boxedTarget});
    Value *isDeftype = Builder.CreateICmpEQ(
        runtimeType, ConstantInt::get(types.i32Ty, deftypeType));

    BasicBlock *deftypeBB =
        BasicBlock::Create(TheContext, "hi_deftype", parentFn);
    BasicBlock *generalBB =
        BasicBlock::Create(TheContext, "hi_general", parentFn);
    BasicBlock *mergeBB =
        BasicBlock::Create(TheContext, "hi_merge", parentFn);

    Builder.CreateCondBr(isDeftype, deftypeBB, generalBB);

    // Deftype path
    Builder.SetInsertPoint(deftypeBB);
    Value *targetPtr = valueEncoder.unboxPointer(
        TypedValue(ObjectTypeSet::dynamicType(), boxedTarget)).value;
    Value *fieldNameStr =
        Builder.CreateGlobalStringPtr(memberName, "field_name");
    FunctionType *getFieldFT = FunctionType::get(
        types.RT_valueTy, {types.ptrTy, types.ptrTy}, false);
    Value *fieldVal = invokeManager.invokeRaw(
        "Deftype_getFieldByName", getFieldFT, {targetPtr, fieldNameStr});
    BasicBlock *deftypeExitBB = Builder.GetInsertBlock();
    Builder.CreateBr(mergeBB);

    // General path
    Builder.SetInsertPoint(generalBB);
    auto generalResult = invokeManager.generateInstanceCall(
        memberName, targetVal, {}, &guard, &node);
    Value *generalVal = valueEncoder.box(generalResult).value;
    BasicBlock *generalExitBB = Builder.GetInsertBlock();
    Builder.CreateBr(mergeBB);

    Builder.SetInsertPoint(mergeBB);
    PHINode *phi = Builder.CreatePHI(types.RT_valueTy, 2, "hi_result");
    phi->addIncoming(fieldVal, deftypeExitBB);
    phi->addIncoming(generalVal, generalExitBB);
    return TypedValue(ObjectTypeSet::dynamicType(), phi);
  }

  return invokeManager.generateInstanceCall(memberName, targetVal, {},
                                            &guard, &node);
}

ObjectTypeSet CodeGen::getType(const Node &node, const HostInteropNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  if (subnode.isassignable()) {
    return ObjectTypeSet::all();
  }

  auto targetType = getType(subnode.target(), ObjectTypeSet::all());
  return this->invokeManager.predictInstanceCallType(subnode.morf(), targetType,
                                                     {});
}

} // namespace rt
