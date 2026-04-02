#include "../CodeGen.h"

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

TypedValue CodeGen::codegen(const Node &node, const LocalNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  switch (subnode.local()) {
  case localTypeArg:
  case localTypeLet:
  case localTypeLetfn:
  case localTypeFn:
  case localTypeLoop:
  case localTypeCatch:
  case localTypeThis: {
    auto name = subnode.name();
    auto *val = variableBindingStack.find(name);
    if (!val) {
      throwCodeGenerationException(string("Unknown variable: ") + name, node);
    }
    return *val;
  }
  case localTypeField: {
    // Field access on 'this' - generate Deftype_getFieldByName call
    auto *thisVal = variableBindingStack.find("this");
    if (!thisVal) {
      throwCodeGenerationException(
          "Field access requires 'this' in scope", node);
    }
    Value *boxedThis = valueEncoder.box(*thisVal).value;
    Value *thisPtr = valueEncoder.unboxPointer(
        TypedValue(ObjectTypeSet::dynamicType(), boxedThis)).value;
    string fieldName = stripUniquifySuffix(subnode.name());
    Value *fieldNameStr = Builder.CreateGlobalStringPtr(fieldName, "field_name");
    FunctionType *getFieldFT = FunctionType::get(
        types.RT_valueTy, {types.ptrTy, types.ptrTy}, false);
    Value *fieldVal = invokeManager.invokeRaw(
        "Deftype_getFieldByName", getFieldFT, {thisPtr, fieldNameStr});
    return TypedValue(ObjectTypeSet::dynamicType(), fieldVal);
  }
  default:
    throwCodeGenerationException(
        string("Compiler does not fully support the following local type yet: ") +
            to_string(subnode.local()),
        node);
  }
}

ObjectTypeSet CodeGen::getType(const Node &node, const LocalNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  switch (subnode.local()) {
  case localTypeArg:
  case localTypeLet:
  case localTypeLetfn:
  case localTypeFn:
  case localTypeLoop:
  case localTypeCatch:
  case localTypeThis: {
    auto name = subnode.name();
    auto *type = variableTypesBindingsStack.find(name);
    if (!type) {
      throwCodeGenerationException(string("Unknown variable: ") + name, node);
    }
    return *type;
  }
  case localTypeField:
    return ObjectTypeSet::dynamicType();
  default:
    throwCodeGenerationException(
        string("Compiler does not fully support the following local type yet: ") +
            to_string(subnode.local()),
        node);
  }
}

} // namespace rt
