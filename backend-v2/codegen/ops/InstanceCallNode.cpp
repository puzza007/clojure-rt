#include "../../tools/EdnParser.h"
#include "../../tools/RTValueWrapper.h"
#include "../../types/ConstantBool.h"
#include "../../types/ConstantDouble.h"
#include "../../types/ConstantInteger.h"
#include "../CodeGen.h"
#include "bridge/Exceptions.h"
#include "bytecode.pb.h"
#include "codegen/TypedValue.h"
#include <sstream>

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

TypedValue CodeGen::codegen(const Node &node, const InstanceCallNode &subnode,
                             const ObjectTypeSet &typeRestrictions) {
  CleanupChainGuard guard(*this);

  auto instanceVal = codegen(subnode.instance(), ObjectTypeSet::all());
  // Retain the instance to protect against premature dropMemory releases.
  // The frontend may annotate a drop on this node for the instance variable,
  // which would be emitted before the call. The retain ensures the object
  // survives through the method dispatch.
  memoryManagement.dynamicRetain(instanceVal);
  guard.push(instanceVal);

  std::vector<TypedValue> args;
  for (int i = 0; i < subnode.args_size(); i++) {
    auto t = codegen(subnode.args(i), ObjectTypeSet::all());
    args.push_back(t);
    guard.push(t);
  }

  return invokeManager.generateInstanceCall(
      subnode.method(), instanceVal, args, &guard, &node);
}

ObjectTypeSet CodeGen::getType(const Node &node,
                               const InstanceCallNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  auto instanceType = getType(subnode.instance(), ObjectTypeSet::all());
  std::vector<ObjectTypeSet> args;
  for (int i = 0; i < subnode.args_size(); i++) {
    args.push_back(getType(subnode.args(i), ObjectTypeSet::all()).unboxed());
  }

  return this->invokeManager.predictInstanceCallType(subnode.method(), instanceType, args);
}

} // namespace rt
