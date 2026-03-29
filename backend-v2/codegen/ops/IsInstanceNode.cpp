#include "../CodeGen.h"

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

// Map Java/Clojure class names to runtime objectType enum values.
// Returns -1 if the class is not a known primitive/built-in type.
static int classNameToObjectType(const string &className) {
  // Strip "class " prefix if present
  string name = className;
  if (name.rfind("class ", 0) == 0)
    name = name.substr(6);

  if (name == "java.lang.Long" || name == "long" || name == "Long")
    return integerType;
  if (name == "java.lang.Double" || name == "double" || name == "Double")
    return doubleType;
  if (name == "java.lang.Boolean" || name == "boolean" || name == "Boolean" ||
      name == "clojure.lang.Boolean")
    return booleanType;
  if (name == "java.lang.String" || name == "String")
    return stringType;
  if (name == "clojure.lang.Keyword" || name == "Keyword")
    return keywordType;
  if (name == "clojure.lang.Symbol" || name == "Symbol")
    return symbolType;
  if (name == "clojure.lang.PersistentVector" || name == "PersistentVector")
    return persistentVectorType;
  if (name == "clojure.lang.PersistentList" || name == "PersistentList")
    return persistentListType;
  if (name == "clojure.lang.PersistentArrayMap" || name == "PersistentArrayMap")
    return persistentArrayMapType;
  if (name == "clojure.lang.BigInt" || name == "BigInt")
    return bigIntegerType;
  if (name == "clojure.lang.Ratio" || name == "Ratio")
    return ratioType;
  if (name == "clojure.lang.Var" || name == "Var")
    return varType;
  if (name == "clojure.lang.IFn" || name == "IFn")
    return functionType;
  return -1;
}

TypedValue CodeGen::codegen(const Node &node, const IsInstanceNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  auto target = codegen(subnode.target(), ObjectTypeSet::all());

  int expectedType = classNameToObjectType(subnode.class_());

  if (expectedType < 0) {
    // Unknown class -- for now return false
    // TODO: support deftype/class hierarchy checks
    return TypedValue(ObjectTypeSet(booleanType, false),
                      ConstantInt::get(types.i1Ty, 0));
  }

  // Static optimization: if target type is known at compile time
  if (target.type.isDetermined()) {
    bool matches = ((int)target.type.determinedType() == expectedType);
    return TypedValue(ObjectTypeSet(booleanType, false),
                      ConstantInt::get(types.i1Ty, matches ? 1 : 0));
  }

  // Dynamic: call getType(v) and compare
  Value *boxedTarget = valueEncoder.box(target).value;
  FunctionType *getTypeFT =
      FunctionType::get(types.i32Ty, {types.RT_valueTy}, false);
  Value *runtimeType =
      invokeManager.invokeRaw("getType", getTypeFT, {boxedTarget});
  Value *result = Builder.CreateICmpEQ(
      runtimeType, ConstantInt::get(types.i32Ty, expectedType), "isinstance");

  return TypedValue(ObjectTypeSet(booleanType, false), result);
}

ObjectTypeSet CodeGen::getType(const Node &node, const IsInstanceNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet(booleanType, false);
}

} // namespace rt
