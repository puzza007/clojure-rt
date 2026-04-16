#include "../CodeGen.h"
#include "../../jit/JITEngine.h"
#include "runtime/Class.h"
#include "runtime/Deftype.h"
#include "tools/EdnParser.h"
#include "tools/RTValueWrapper.h"

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

TypedValue CodeGen::codegen(const Node &node, const DeftypeNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  // Extract field names
  int fieldCount = subnode.fields_size();
  vector<string> fieldNames;
  for (int i = 0; i < fieldCount; i++) {
    fieldNames.push_back(
        stripUniquifySuffix(subnode.fields(i).subnode().binding().name()));
  }

  // Create the class at compile time (C++ side) and register it
  auto *desc = new ClassDescription();
  desc->type = ObjectTypeSet(deftypeType);
  desc->name = subnode.name();

  string className = subnode.classname();
  if (className.rfind("class ", 0) == 0)
    className = className.substr(6);

  for (int i = 0; i < fieldCount; i++) {
    desc->instanceFields[fieldNames[i]] = i;
  }

  // Create runtime Class object
  String *nameStr = String_createDynamicStr(subnode.name().c_str());
  String *classNameStr = String_createDynamicStr(className.c_str());
  Class *cls = Class_create(nameStr, classNameStr, 0, nullptr);
  cls->compilerExtension = desc;
  cls->compilerExtensionDestructor = delete_class_description;

  // Compile field getters and method implementations via separate JIT modules
  // so they survive optimization and are available as symbols for bridges.
  // All compilations are launched first, then awaited in batch.
  if (jitEnginePtr) {
    auto *jit = static_cast<JITEngine *>(jitEnginePtr);
    string safeClassName = className;
    replace(safeClassName.begin(), safeClassName.end(), '.', '_');

    struct PendingCompilation {
      string name;  // instanceFns key
      IntrinsicDescription intrinsic;
      std::shared_future<JITResult> future;
    };
    vector<PendingCompilation> pending;

    // Launch getter compilations for each field
    for (int i = 0; i < fieldCount; i++) {
      string fname = fieldNames[i];
      string funcName = safeClassName + "_get_" + fname;

      IntrinsicDescription getter;
      getter.type = CallType::Call;
      getter.isInstance = true;
      getter.thisType = ObjectTypeSet(deftypeType);
      getter.returnType = ObjectTypeSet::dynamicType();
      getter.argTypes.push_back(ObjectTypeSet::dynamicType());
      getter.symbol = funcName;

      auto future = jit->compileGeneric(
          [funcName, fname](CodeGen &cg) -> string {
            auto &ctx = cg.getContext();
            auto &mod = cg.getModule();
            auto &builder = cg.getBuilder();
            auto &types = cg.getLLVMTypes();

            vector<Type *> paramTypes = {types.RT_valueTy};
            FunctionType *FT =
                FunctionType::get(types.RT_valueTy, paramTypes, false);
            Function *F = Function::Create(FT, Function::ExternalLinkage,
                                           funcName, mod);
            BasicBlock *entry = BasicBlock::Create(ctx, "entry", F);
            builder.SetInsertPoint(entry);

            Value *thisVal = &*F->arg_begin();
            Value *thisPtr = cg.getValueEncoder().unboxPointer(
                TypedValue(ObjectTypeSet::dynamicType(), thisVal)).value;

            FunctionType *getFieldFT = FunctionType::get(
                types.RT_valueTy, {types.ptrTy, types.ptrTy}, false);
            Value *fnameStr = builder.CreateGlobalStringPtr(fname);
            Value *fieldVal = builder.CreateCall(
                mod.getOrInsertFunction("Deftype_getFieldByName", getFieldFT),
                {thisPtr, fnameStr});
            builder.CreateRet(fieldVal);
            return funcName;
          },
          funcName, llvm::OptimizationLevel::O0, false, true);

      pending.push_back({fname, getter, future});
    }

    // Launch method compilations
    for (int i = 0; i < subnode.methods_size(); i++) {
      const auto &methodAst = subnode.methods(i);
      const auto &methodNode = methodAst.subnode().method();
      string methName = methodNode.name();

      IntrinsicDescription intrinsic;
      intrinsic.type = CallType::Call;
      intrinsic.isInstance = true;
      intrinsic.thisType = ObjectTypeSet(deftypeType);
      intrinsic.returnType = ObjectTypeSet::dynamicType();
      intrinsic.argTypes.push_back(ObjectTypeSet::dynamicType());
      for (int p = 0; p < methodNode.params_size(); p++) {
        auto &binding = methodNode.params(p).subnode().binding();
        if (binding.local() == localTypeThis ||
            binding.local() == localTypeFn ||
            binding.local() == localTypeField)
          continue;
        intrinsic.argTypes.push_back(ObjectTypeSet::dynamicType());
      }

      string funcName = safeClassName + "_" + methName + "_" +
                        to_string(intrinsic.argTypes.size() - 1);
      intrinsic.symbol = funcName;

      auto methodAstCopy = make_shared<Node>(methodAst);
      int argCount = (int)intrinsic.argTypes.size();

      auto future = jit->compileGeneric(
          [funcName, methodAstCopy, argCount](CodeGen &cg) -> string {
            auto &ctx = cg.getContext();
            auto &mod = cg.getModule();
            auto &builder = cg.getBuilder();
            auto &types = cg.getLLVMTypes();

            const auto &methodNode = methodAstCopy->subnode().method();

            vector<Type *> paramTypes(argCount, types.RT_valueTy);
            FunctionType *FT =
                FunctionType::get(types.RT_valueTy, paramTypes, false);
            Function *F = Function::Create(FT, Function::ExternalLinkage,
                                           funcName, mod);
            F->addFnAttr("frame-pointer", "non-leaf");

            FunctionType *personalityFnTy =
                FunctionType::get(types.i32Ty, true);
            auto pFn = mod.getOrInsertFunction("__gxx_personality_v0",
                                               personalityFnTy);
            F->setPersonalityFn(cast<Function>(pFn.getCallee()));

            BasicBlock *entry = BasicBlock::Create(ctx, "entry", F);
            builder.SetInsertPoint(entry);

            cg.getMemoryManagement().initFunction(F);
            cg.getVariableBindingStack().push();
            cg.getVariableTypesBindingsStack().push();

            auto argIt = F->arg_begin();
            TypedValue thisTV(ObjectTypeSet::dynamicType(), &*argIt++);

            cg.getVariableBindingStack().set("this", thisTV);
            cg.getVariableTypesBindingsStack().set(
                "this", ObjectTypeSet::dynamicType());

            if (methodNode.has_this_()) {
              string thisName = methodNode.this_().subnode().binding().name();
              cg.getVariableBindingStack().set(thisName, thisTV);
              cg.getVariableTypesBindingsStack().set(
                  thisName, ObjectTypeSet::dynamicType());
            }

            for (int p = 0; p < methodNode.params_size(); p++) {
              auto &binding = methodNode.params(p).subnode().binding();
              if (binding.local() == localTypeThis ||
                  binding.local() == localTypeFn ||
                  binding.local() == localTypeField)
                continue;
              string pname = binding.name();
              TypedValue paramTV(ObjectTypeSet::dynamicType(), &*argIt++);
              cg.getVariableBindingStack().set(pname, paramTV);
              cg.getVariableTypesBindingsStack().set(
                  pname, ObjectTypeSet::dynamicType());
            }

            // Register recur context so recur inside method bodies works
            string loopId = methodNode.loopid();
            cg.fnRecurContexts[loopId] =
                CodeGen::FnRecurContext{F};
            cg.recurContextTypes[loopId] =
                CodeGen::RecurContextType::Method;

            auto bodyResult =
                cg.codegen(methodNode.body(), ObjectTypeSet::all());

            if (bodyResult.value != nullptr) {
              builder.CreateRet(cg.getValueEncoder().box(bodyResult).value);
            }

            for (auto &BB : *F) {
              if (!BB.getTerminator()) {
                builder.SetInsertPoint(&BB);
                builder.CreateUnreachable();
              }
            }

            cg.recurContextTypes.erase(loopId);
            cg.fnRecurContexts.erase(loopId);
            cg.getVariableBindingStack().pop();
            cg.getVariableTypesBindingsStack().pop();

            return funcName;
          },
          funcName, llvm::OptimizationLevel::O0, false, true);

      pending.push_back({methName, intrinsic, future});
    }

    // Await all compilations and register intrinsics
    for (auto &p : pending) {
      p.future.get();
      desc->instanceFns[p.name].push_back(p.intrinsic);
    }
  }

  // Register by name only — deftypeType index is shared across all deftypes,
  // so indexed registration would cause the last deftype to shadow earlier ones.
  compilerState.classRegistry.registerObject(className.c_str(), cls);

  // Emit LLVM IR that returns nil (deftype is a side-effect form)
  return dynamicConstructor.createNil();
}

ObjectTypeSet CodeGen::getType(const Node &node, const DeftypeNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet(nilType);
}

// MethodNode cannot be compiled standalone
TypedValue CodeGen::codegen(const Node &node, const MethodNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  throwCodeGenerationException(
      "MethodNode must be compiled as part of DeftypeNode or ReifyNode", node);
  return TypedValue(ObjectTypeSet::all(), nullptr);
}

ObjectTypeSet CodeGen::getType(const Node &node, const MethodNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet::all();
}

} // namespace rt
