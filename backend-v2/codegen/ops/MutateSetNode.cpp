#include "../CodeGen.h"

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

TypedValue CodeGen::codegen(const Node &node, const MutateSetNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  // set! on a deftype mutable field: target is a local (field) binding,
  // which in method context points to 'this'. We need to set the field
  // on the deftype instance.

  // Get the field name from the target local node
  const auto &targetNode = subnode.target();
  string fieldName;
  if (targetNode.op() == opLocal) {
    fieldName = stripUniquifySuffix(targetNode.subnode().local().name());
  } else {
    throwCodeGenerationException(
        "set! target must be a local binding for mutable field", node);
  }

  // Compile the new value
  auto newVal = codegen(subnode.val(), ObjectTypeSet::all());

  // Get 'this' from the variable binding stack
  auto *thisTV = variableBindingStack.find("this");
  if (!thisTV) {
    throwCodeGenerationException(
        "set! on mutable field requires 'this' in scope", node);
  }

  // Box the new value and this
  Value *boxedVal = valueEncoder.box(newVal).value;
  Value *boxedThis = valueEncoder.box(*thisTV).value;
  Value *thisPtr = valueEncoder.unboxPointer(
      TypedValue(ObjectTypeSet::dynamicType(), boxedThis)).value;
  Value *fieldNameStr = Builder.CreateGlobalStringPtr(fieldName, "set_field_name");

  // Call Deftype_setFieldByName(instance, fieldName, value)
  FunctionType *setFieldFT = FunctionType::get(
      types.voidTy, {types.ptrTy, types.ptrTy, types.RT_valueTy}, false);
  invokeManager.invokeRaw("Deftype_setFieldByName", setFieldFT,
                          {thisPtr, fieldNameStr, boxedVal});

  return newVal;
}

ObjectTypeSet CodeGen::getType(const Node &node, const MutateSetNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return getType(subnode.val(), typeRestrictions);
}

} // namespace rt
