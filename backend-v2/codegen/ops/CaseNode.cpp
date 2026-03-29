#include "../CodeGen.h"

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

TypedValue CodeGen::codegen(const Node &node, const CaseNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  Function *parentFn = Builder.GetInsertBlock()->getParent();

  auto testVal = codegen(subnode.test(), ObjectTypeSet::all());

  Value *switchVal;
  bool needsEqualityCheck = false;

  if (subnode.testtype() == CaseNode::testTypeInt) {
    switchVal = valueEncoder.unboxInt32(testVal).value;
    switchVal = Builder.CreateSExt(switchVal, types.i64Ty, "case_ext");
  } else {
    // Hash-based dispatch: compute Java .hashCode() equivalent
    needsEqualityCheck = true;
    Value *boxedTest = valueEncoder.box(testVal).value;
    FunctionType *hashFT =
        FunctionType::get(types.i32Ty, {types.RT_valueTy}, false);
    Value *hashVal = invokeManager.invokeRaw("javaHashCode", hashFT, {boxedTest});

    if (subnode.switchtype() == CaseNode::switchTypeCompact) {
      // Compact mode: index = (hash >> shift) & mask
      int32_t shift = subnode.shift();
      int32_t mask = subnode.mask();
      Value *shifted = Builder.CreateAShr(hashVal,
          ConstantInt::get(types.i32Ty, shift), "case_shifted");
      Value *masked = Builder.CreateAnd(shifted,
          ConstantInt::get(types.i32Ty, mask), "case_masked");
      switchVal = Builder.CreateSExt(masked, types.i64Ty, "case_idx");
    } else {
      // Sparse mode: switch directly on hash
      switchVal = Builder.CreateSExt(hashVal, types.i64Ty, "case_hash_ext");
    }
  }

  BasicBlock *mergeBB =
      BasicBlock::Create(TheContext, "case_merge", parentFn);
  BasicBlock *defaultBB =
      BasicBlock::Create(TheContext, "case_default", parentFn);

  int numCases = subnode.tests_size();
  SwitchInst *sw = Builder.CreateSwitch(switchVal, defaultBB, numCases);

  struct CaseBranch {
    BasicBlock *exitBB;
    Value *result;
  };
  vector<CaseBranch> branches;

  FunctionType *eqFT = needsEqualityCheck
      ? FunctionType::get(types.i1Ty, {types.RT_valueTy, types.RT_valueTy}, false)
      : nullptr;

  for (int i = 0; i < numCases; i++) {
    auto &caseTest = subnode.tests(i).subnode().casetest();
    auto &caseThen = subnode.thens(i).subnode().casethen();

    int32_t hash = caseTest.hash();

    BasicBlock *thenBB = BasicBlock::Create(
        TheContext, "case_" + to_string(hash), parentFn);
    sw->addCase(
        cast<ConstantInt>(ConstantInt::get(types.i64Ty, (int64_t)hash)),
        thenBB);

    Builder.SetInsertPoint(thenBB);

    if (needsEqualityCheck) {
      auto caseConst = codegen(caseTest.test(), ObjectTypeSet::all());
      Value *boxedTest = valueEncoder.box(testVal).value;
      Value *boxedConst = valueEncoder.box(caseConst).value;
      Value *isEqual =
          invokeManager.invokeRaw("equals", eqFT, {boxedTest, boxedConst});

      BasicBlock *matchBB = BasicBlock::Create(
          TheContext, "case_match_" + to_string(i), parentFn);
      Builder.CreateCondBr(isEqual, matchBB, defaultBB);
      Builder.SetInsertPoint(matchBB);
    }

    TypedValue thenResult = codegen(caseThen.then(), typeRestrictions);

    if (thenResult.value != nullptr) {
      Value *boxed = valueEncoder.box(thenResult).value;
      branches.push_back({Builder.GetInsertBlock(), boxed});
      Builder.CreateBr(mergeBB);
    } else {
      Builder.CreateUnreachable();
    }
  }

  // Default branch
  Builder.SetInsertPoint(defaultBB);
  TypedValue defaultResult = codegen(subnode.default_(), typeRestrictions);
  if (defaultResult.value != nullptr) {
    Value *boxed = valueEncoder.box(defaultResult).value;
    branches.push_back({Builder.GetInsertBlock(), boxed});
    Builder.CreateBr(mergeBB);
  } else {
    Builder.CreateUnreachable();
  }

  // Merge
  Builder.SetInsertPoint(mergeBB);
  if (branches.empty()) {
    return TypedValue(ObjectTypeSet::all(), nullptr);
  }

  PHINode *phi =
      Builder.CreatePHI(types.RT_valueTy, branches.size(), "case_result");
  for (auto &b : branches)
    phi->addIncoming(b.result, b.exitBB);

  return TypedValue(ObjectTypeSet::dynamicType(), phi);
}

TypedValue CodeGen::codegen(const Node &node, const CaseTestNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  throwCodeGenerationException(
      "CaseTestNode must be compiled as part of CaseNode", node);
  return TypedValue(ObjectTypeSet::all(), nullptr);
}

TypedValue CodeGen::codegen(const Node &node, const CaseThenNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  throwCodeGenerationException(
      "CaseThenNode must be compiled as part of CaseNode", node);
  return TypedValue(ObjectTypeSet::all(), nullptr);
}

ObjectTypeSet CodeGen::getType(const Node &node, const CaseNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet::all();
}

ObjectTypeSet CodeGen::getType(const Node &node, const CaseTestNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet::all();
}

ObjectTypeSet CodeGen::getType(const Node &node, const CaseThenNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet::all();
}

} // namespace rt
