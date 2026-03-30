#include "../CodeGen.h"

using namespace clojure::rt::protobuf::bytecode;

namespace rt {
TypedValue CodeGen::codegen(const Node &node, const BindingNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  if (subnode.has_init()) {
    return codegen(subnode.init(), typeRestrictions);
  }
  return dynamicConstructor.createNil();
}

ObjectTypeSet CodeGen::getType(const Node &node, const BindingNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  if (subnode.has_init()) {
    return getType(subnode.init(), typeRestrictions);
  }
  return ObjectTypeSet(nilType).restriction(typeRestrictions);
}
} // namespace rt
