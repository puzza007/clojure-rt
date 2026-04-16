#include "../CodeGen.h"

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

TypedValue CodeGen::codegen(const Node &node, const KeywordInvokeNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  auto keyword = codegen(subnode.keyword(), ObjectTypeSet::all());
  auto target = codegen(subnode.target(), ObjectTypeSet::all());

  if (target.type.isDetermined() &&
      target.type.determinedType() == persistentArrayMapType) {
    auto retType = ObjectTypeSet::dynamicType();
    return invokeManager.invokeRuntime(
        "PersistentArrayMap_get", &retType,
        {ObjectTypeSet(persistentArrayMapType), ObjectTypeSet::dynamicType()},
        {target, keyword});
  }

  // Dynamic path: dispatches to map get, deftype field access, or set lookup
  auto retType = ObjectTypeSet::dynamicType();
  return invokeManager.invokeRuntime(
      "core_keyword_invoke", &retType,
      {ObjectTypeSet::dynamicType(), ObjectTypeSet::dynamicType()},
      {target, keyword});
}

ObjectTypeSet CodeGen::getType(const Node &node,
                               const KeywordInvokeNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet::all();
}

} // namespace rt
