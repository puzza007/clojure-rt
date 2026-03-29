#include "../CodeGen.h"

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

TypedValue CodeGen::codegen(const Node &node, const InstanceFieldNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  // Instance field access requires deftype support (field layout on user types).
  // Will be fully implemented alongside opDeftype.
  throwCodeGenerationException(
      "Instance field access requires deftype (not yet implemented): ." +
          subnode.field(),
      node);
  return TypedValue(ObjectTypeSet::all(), nullptr);
}

ObjectTypeSet CodeGen::getType(const Node &node,
                               const InstanceFieldNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet::all();
}

} // namespace rt
