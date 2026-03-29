#include "../CodeGen.h"
#include "runtime/Deftype.h"
#include "tools/EdnParser.h"
#include "tools/RTValueWrapper.h"
#include <cstddef>

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

static constexpr size_t kDeftypeValuesOffset = offsetof(Deftype, values);

TypedValue CodeGen::codegen(const Node &node, const InstanceFieldNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  auto instance = codegen(subnode.instance(), ObjectTypeSet::all());
  string fieldName = subnode.field();

  // Dynamic field access via runtime helper
  // Unbox instance to get the Deftype pointer
  Value *boxedInstance = valueEncoder.box(instance).value;
  Value *instancePtr =
      valueEncoder
          .unboxPointer(TypedValue(ObjectTypeSet::dynamicType(), boxedInstance))
          .value;

  // Look up field index at compile time if we know the class
  string className = subnode.class_();
  if (className.rfind("class ", 0) == 0)
    className = className.substr(6);

  PtrWrapper<Class> cls(
      compilerState.classRegistry.getCurrent(className.c_str()));
  if (cls) {
    auto *ext = static_cast<ClassDescription *>(cls->compilerExtension);
    if (ext) {
      auto it = ext->instanceFields.find(fieldName);
      if (it != ext->instanceFields.end()) {
        int32_t fieldIndex = it->second;

        // Load values[fieldIndex] directly via pointer arithmetic
        Value *baseAddr =
            Builder.CreatePtrToInt(instancePtr, types.i64Ty);
        size_t fieldOffset =
            kDeftypeValuesOffset + fieldIndex * sizeof(RTValue);
        Value *fieldAddr = Builder.CreateAdd(
            baseAddr, ConstantInt::get(types.i64Ty, fieldOffset));
        Value *fieldPtr =
            Builder.CreateIntToPtr(fieldAddr, types.ptrTy);
        Value *fieldVal =
            Builder.CreateLoad(types.RT_valueTy, fieldPtr, "field_" + fieldName);

        return TypedValue(ObjectTypeSet::dynamicType(), fieldVal);
      }
    }
  }

  // Fallback: field not found at compile time
  throwCodeGenerationException(
      "Cannot resolve field ." + fieldName + " on " + className, node);
  return TypedValue(ObjectTypeSet::all(), nullptr);
}

ObjectTypeSet CodeGen::getType(const Node &node,
                               const InstanceFieldNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet::all();
}

} // namespace rt
