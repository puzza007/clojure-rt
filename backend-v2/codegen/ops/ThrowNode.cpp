#include "../CodeGen.h"

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

TypedValue CodeGen::codegen(const Node &node, const ThrowNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  auto exnValue = codegen(subnode.exception(), ObjectTypeSet::all());

  Value *nameStr = Builder.CreateGlobalStringPtr("Exception", "throw_name");
  TypedValue nameTV(ObjectTypeSet(stringType), nameStr);
  TypedValue nilTV = dynamicConstructor.createNil();

  // throwLanguageException_C(name, message, payload) — does not return
  FunctionType *throwFT = FunctionType::get(
      types.voidTy, {types.ptrTy, types.RT_valueTy, types.RT_valueTy}, false);
  invokeManager.invokeRaw("throwLanguageException_C", throwFT,
      {nameStr, valueEncoder.box(exnValue).value,
       valueEncoder.box(nilTV).value});
  Builder.CreateUnreachable();

  // Dead BB for subsequent code
  Function *parentFn = Builder.GetInsertBlock()->getParent();
  BasicBlock *deadBB =
      BasicBlock::Create(TheContext, "post_throw", parentFn);
  Builder.SetInsertPoint(deadBB);

  return TypedValue(ObjectTypeSet::all(), nullptr);
}

ObjectTypeSet CodeGen::getType(const Node &node, const ThrowNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet::all();
}

} // namespace rt
