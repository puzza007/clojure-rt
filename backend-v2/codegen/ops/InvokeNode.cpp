#include "../CodeGen.h"

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

TypedValue CodeGen::codegen(const Node &node, const InvokeNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  // Compile the function expression
  auto fnExpr = codegen(subnode.fn(), ObjectTypeSet::all());

  // Compile all arguments
  vector<TypedValue> args;
  for (int i = 0; i < subnode.args_size(); i++) {
    args.push_back(codegen(subnode.args(i), ObjectTypeSet::all()));
  }

  // Box fn to RTValue and extract the raw pointer
  Value *fnRTValue = valueEncoder.box(fnExpr).value;
  Value *fnRawPtr =
      valueEncoder
          .unboxPointer(TypedValue(ObjectTypeSet::dynamicType(), fnRTValue))
          .value;

  // Find the matching method implementation via runtime helper
  FunctionType *getImplFT =
      FunctionType::get(types.ptrTy, {types.ptrTy, types.i64Ty}, false);
  Value *implPtr = invokeManager.invokeRaw(
      "Function_getBaselineImpl", getImplFT,
      {fnRawPtr, ConstantInt::get(types.i64Ty, args.size())});

  // Build the indirect call: all args boxed + fn object (boxed) as last arg
  vector<Value *> callArgs;
  for (auto &arg : args) {
    callArgs.push_back(valueEncoder.box(arg).value);
  }
  callArgs.push_back(fnRTValue); // fn object as last arg

  int totalParams = args.size() + 1;
  vector<Type *> callParamTypes(totalParams, types.RT_valueTy);
  FunctionType *callFT =
      FunctionType::get(types.RT_valueTy, callParamTypes, false);

  Value *result = invokeManager.invokeRaw(implPtr, callFT, callArgs);
  return TypedValue(ObjectTypeSet::dynamicType(), result);
}

ObjectTypeSet CodeGen::getType(const Node &node, const InvokeNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet::all();
}

} // namespace rt
