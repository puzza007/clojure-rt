#include "../CodeGen.h"
#include "runtime/Class.h"
#include "runtime/Deftype.h"
#include "tools/EdnParser.h"
#include "tools/RTValueWrapper.h"
#include <cstddef>

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

TypedValue CodeGen::codegen(const Node &node, const NewNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  // Get class name from the const node
  string className = subnode.class_().subnode().const_().val();
  if (className.rfind("class ", 0) == 0)
    className = className.substr(6);

  // Look up class in registry
  PtrWrapper<Class> cls(
      compilerState.classRegistry.getCurrent(className.c_str()));
  if (!cls) {
    throwCodeGenerationException("Class not found for new: " + className, node);
  }

  auto *ext = static_cast<ClassDescription *>(cls->compilerExtension);
  if (!ext) {
    throwCodeGenerationException(
        "Class " + className + " has no compiler metadata", node);
  }

  int fieldCount = ext->instanceFields.size();
  if (fieldCount != subnode.args_size()) {
    throwCodeGenerationException(
        "Wrong number of constructor args for " + className + ": expected " +
            to_string(fieldCount) + ", got " + to_string(subnode.args_size()),
        node);
  }

  // Compile constructor arguments
  vector<Value *> boxedArgs;
  for (int i = 0; i < subnode.args_size(); i++) {
    auto arg = codegen(subnode.args(i), ObjectTypeSet::all());
    boxedArgs.push_back(valueEncoder.box(arg).value);
  }

  // Get the Class* as a constant pointer
  uint64_t classAddr = reinterpret_cast<uint64_t>(cls.get());
  Value *classPtr = ConstantExpr::getIntToPtr(
      ConstantInt::get(types.i64Ty, classAddr), types.ptrTy);

  // Call Deftype_create(class, fieldCount, arg0, arg1, ...)
  vector<Type *> paramTypes = {types.ptrTy, types.i64Ty};
  for (int i = 0; i < fieldCount; i++)
    paramTypes.push_back(types.RT_valueTy);
  FunctionType *createFT =
      FunctionType::get(types.ptrTy, paramTypes, true);

  vector<Value *> createArgs;
  createArgs.push_back(classPtr);
  createArgs.push_back(ConstantInt::get(types.i64Ty, fieldCount));
  for (auto *arg : boxedArgs)
    createArgs.push_back(arg);

  Value *instance =
      invokeManager.invokeRaw("Deftype_create", createFT, createArgs);

  return TypedValue(ObjectTypeSet(deftypeType), instance);
}

ObjectTypeSet CodeGen::getType(const Node &node, const NewNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet(deftypeType);
}

} // namespace rt
