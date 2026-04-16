#include "../CodeGen.h"

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

TypedValue CodeGen::codegen(const Node &node, const RecurNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  string loopId = subnode.loopid();

  auto ctxIt = recurContextTypes.find(loopId);
  if (ctxIt == recurContextTypes.end()) {
    throwCodeGenerationException("recur with unknown loop target: " + loopId,
                                 node);
  }

  if (ctxIt->second == RecurContextType::Loop) {
    // Loop context: update PHI nodes and branch back to loop header
    auto &loopCtx = loopContexts[loopId];

    vector<Value *> newValues;
    for (int i = 0; i < subnode.exprs_size(); i++) {
      auto expr = codegen(subnode.exprs(i), ObjectTypeSet::all());
      newValues.push_back(valueEncoder.box(expr).value);
    }

    BasicBlock *currentBB = Builder.GetInsertBlock();
    for (size_t i = 0; i < newValues.size(); i++) {
      loopCtx.phiNodes[i]->addIncoming(newValues[i], currentBB);
    }

    Builder.CreateBr(loopCtx.headerBB);

    // Create dead BB for subsequent code (terminated before module verification)
    Function *parentFn = Builder.GetInsertBlock()->getParent();
    BasicBlock *deadBB =
        BasicBlock::Create(Builder.getContext(), "post_recur", parentFn);
    Builder.SetInsertPoint(deadBB);

    return TypedValue(ObjectTypeSet::all(), nullptr);

  } else if (ctxIt->second == RecurContextType::Fn) {
    // Fn context: tail call back to the fn method
    auto &fnCtx = fnRecurContexts[loopId];

    vector<Value *> recurArgs;
    for (int i = 0; i < subnode.exprs_size(); i++) {
      auto expr = codegen(subnode.exprs(i), ObjectTypeSet::all());
      recurArgs.push_back(valueEncoder.box(expr).value);
    }

    // Last argument is the fn object (last arg of the current LLVM function)
    Function *currentFn = Builder.GetInsertBlock()->getParent();
    auto lastArgIt = currentFn->arg_end();
    --lastArgIt;
    recurArgs.push_back(&*lastArgIt);

    auto *call = Builder.CreateCall(fnCtx.llvmFunction, recurArgs, "recur");
    call->setTailCallKind(CallInst::TCK_Tail);
    Builder.CreateRet(call);

    BasicBlock *deadBB =
        BasicBlock::Create(Builder.getContext(), "post_recur", currentFn);
    Builder.SetInsertPoint(deadBB);

    return TypedValue(ObjectTypeSet::all(), nullptr);

  } else {
    // Method context: tail call back to the method function
    // Args: [this, recur_exprs...] — no trailing fn object
    auto &fnCtx = fnRecurContexts[loopId];
    Function *currentFn = Builder.GetInsertBlock()->getParent();

    vector<Value *> recurArgs;
    // First arg is always 'this' (first LLVM function argument, unchanged)
    recurArgs.push_back(&*currentFn->arg_begin());

    for (int i = 0; i < subnode.exprs_size(); i++) {
      auto expr = codegen(subnode.exprs(i), ObjectTypeSet::all());
      recurArgs.push_back(valueEncoder.box(expr).value);
    }

    auto *call = Builder.CreateCall(fnCtx.llvmFunction, recurArgs, "recur");
    call->setTailCallKind(CallInst::TCK_Tail);
    Builder.CreateRet(call);

    BasicBlock *deadBB =
        BasicBlock::Create(Builder.getContext(), "post_recur", currentFn);
    Builder.SetInsertPoint(deadBB);

    return TypedValue(ObjectTypeSet::all(), nullptr);
  }
}

ObjectTypeSet CodeGen::getType(const Node &node, const RecurNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet::all();
}

} // namespace rt
