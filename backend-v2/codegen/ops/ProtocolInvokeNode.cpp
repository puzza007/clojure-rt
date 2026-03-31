#include "../CodeGen.h"

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

TypedValue CodeGen::codegen(const Node &node,
                            const ProtocolInvokeNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  // Protocol invoke: extract method name and dispatch as instance call.
  // This path is a fallback - normally the frontend desugar-protocol-invoke
  // pass converts protocol invokes to regular invokes.
  CleanupChainGuard guard(*this);

  auto target = codegen(subnode.target(), ObjectTypeSet::all());
  guard.push(target);

  vector<TypedValue> args;
  for (int i = 0; i < subnode.args_size(); i++) {
    auto a = codegen(subnode.args(i), ObjectTypeSet::all());
    args.push_back(a);
    guard.push(a);
  }

  // Extract method name from the protocol function var
  string methodName;
  const auto &fnNode = subnode.protocolfn();
  if (fnNode.op() == opVar) {
    string varName = fnNode.subnode().var().var();
    auto slashPos = varName.rfind('/');
    methodName = (slashPos != string::npos) ? varName.substr(slashPos + 1)
                                            : varName;
  } else if (fnNode.op() == opTheVar) {
    string varName = fnNode.subnode().thevar().var();
    auto slashPos = varName.rfind('/');
    methodName = (slashPos != string::npos) ? varName.substr(slashPos + 1)
                                            : varName;
  } else {
    methodName = node.form();
  }

  return invokeManager.generateInstanceCall(methodName, target, args, &guard,
                                            &node);
}

ObjectTypeSet CodeGen::getType(const Node &node,
                               const ProtocolInvokeNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet::dynamicType();
}

} // namespace rt
