#include "../CodeGen.h"

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

TypedValue CodeGen::codegen(const Node &node, const LoopNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  variableBindingStack.push();
  variableTypesBindingsStack.push();

  Function *parentFn = Builder.GetInsertBlock()->getParent();

  // Compile init values for each binding in the current BB
  vector<Value *> initValues;
  vector<string> bindingNames;
  for (int i = 0; i < subnode.bindings_size(); i++) {
    auto bindingNode = subnode.bindings(i);
    auto binding = bindingNode.subnode().binding();
    auto init = codegen(binding.init(), ObjectTypeSet::all());
    initValues.push_back(valueEncoder.box(init).value);
    bindingNames.push_back(binding.name());
  }

  // Create loop header BB and branch to it
  BasicBlock *preLoopBB = Builder.GetInsertBlock();
  BasicBlock *loopBB = BasicBlock::Create(
      Builder.getContext(), "loop_header", parentFn);
  Builder.CreateBr(loopBB);
  Builder.SetInsertPoint(loopBB);

  // Create PHI nodes (one per binding)
  vector<PHINode *> phiNodes;
  for (size_t i = 0; i < initValues.size(); i++) {
    PHINode *phi = Builder.CreatePHI(types.RT_valueTy, 2, bindingNames[i]);
    phi->addIncoming(initValues[i], preLoopBB);
    phiNodes.push_back(phi);

    TypedValue phiTV(ObjectTypeSet::dynamicType(), phi);
    variableBindingStack.set(bindingNames[i], phiTV);
    variableTypesBindingsStack.set(bindingNames[i], ObjectTypeSet::all());
  }

  // Register loop context for RecurNode
  string loopId = subnode.loopid();
  loopContexts[loopId] = LoopContext{loopBB, phiNodes};
  recurContextTypes[loopId] = RecurContextType::Loop;

  // Compile body
  auto bodyResult = codegen(subnode.body(), typeRestrictions);

  // Clean up
  loopContexts.erase(loopId);
  recurContextTypes.erase(loopId);
  variableBindingStack.pop();
  variableTypesBindingsStack.pop();

  return bodyResult;
}

ObjectTypeSet CodeGen::getType(const Node &node, const LoopNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  variableTypesBindingsStack.push();

  for (int i = 0; i < subnode.bindings_size(); i++) {
    auto bindingNode = subnode.bindings(i);
    auto binding = bindingNode.subnode().binding();
    variableTypesBindingsStack.set(binding.name(), ObjectTypeSet::all());
  }

  string loopId = subnode.loopid();
  recurContextTypes[loopId] = RecurContextType::Loop;

  auto retVal = getType(subnode.body(), typeRestrictions);

  recurContextTypes.erase(loopId);
  variableTypesBindingsStack.pop();
  return retVal;
}

} // namespace rt
