#include "../CodeGen.h"
#include "../../jit/JITEngine.h"
#include "runtime/Class.h"
#include "runtime/Deftype.h"
#include "tools/EdnParser.h"

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

TypedValue CodeGen::codegen(const Node &node, const ReifyNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  // Reify is an anonymous deftype that captures closed-over variables.
  // Closed-overs become implicit fields of a Deftype instance. Methods are
  // compiled in separate JIT modules (like DeftypeNode) and load closed-overs
  // from the Deftype fields via Deftype_getFieldByName.

  string className = subnode.classname();
  if (className.rfind("class ", 0) == 0)
    className = className.substr(6);

  // Collect closed-over names and evaluate their values in the outer scope
  int closedOverCount = subnode.closedovers_size();
  vector<string> closedOverNames;
  vector<Value *> closedOverValues;
  for (int i = 0; i < closedOverCount; i++) {
    auto &coNode = subnode.closedovers(i);
    closedOverNames.push_back(coNode.subnode().local().name());
    auto coVal = codegen(coNode, ObjectTypeSet::all());
    closedOverValues.push_back(valueEncoder.box(coVal).value);
  }

  // Create ClassDescription with closed-overs as fields
  auto *desc = new ClassDescription();
  desc->type = ObjectTypeSet(deftypeType);
  desc->name = className;

  for (int i = 0; i < closedOverCount; i++) {
    desc->instanceFields[closedOverNames[i]] = i;
  }

  // Create runtime Class object
  String *nameStr = String_createDynamicStr(className.c_str());
  String *classNameStr = String_createDynamicStr(className.c_str());
  Class *cls = Class_create(nameStr, classNameStr, 0, nullptr);
  cls->compilerExtension = desc;
  cls->compilerExtensionDestructor = delete_class_description;

  if (jitEnginePtr) {
    auto *jit = static_cast<JITEngine *>(jitEnginePtr);
    string safeClassName = className;
    replace(safeClassName.begin(), safeClassName.end(), '.', '_');

    struct PendingCompilation {
      string name;
      IntrinsicDescription intrinsic;
      std::shared_future<JITResult> future;
    };
    vector<PendingCompilation> pending;

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
      auto coNames = make_shared<vector<string>>(closedOverNames);
      size_t valuesOffset = offsetof(Deftype, values);

      auto future = jit->compileGeneric(
          [funcName, methodAstCopy, argCount, coNames,
           valuesOffset](CodeGen &cg) -> string {
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

            // Load closed-overs directly from Deftype fields via pointer
            // arithmetic (O(1) per field, vs O(n) name-based lookup).
            if (!coNames->empty()) {
              Value *thisPtr = cg.getValueEncoder()
                                   .unboxPointer(thisTV).value;
              Value *baseAddr =
                  builder.CreatePtrToInt(thisPtr, types.i64Ty);

              for (size_t j = 0; j < coNames->size(); j++) {
                size_t offset = valuesOffset + j * sizeof(RTValue);
                Value *fieldAddr = builder.CreateAdd(
                    baseAddr, ConstantInt::get(types.i64Ty, offset));
                Value *fieldPtr =
                    builder.CreateIntToPtr(fieldAddr, types.ptrTy);
                Value *fieldVal = builder.CreateLoad(
                    types.RT_valueTy, fieldPtr,
                    "co_" + (*coNames)[j]);
                TypedValue coTV(ObjectTypeSet::dynamicType(), fieldVal);
                cg.getVariableBindingStack().set((*coNames)[j], coTV);
                cg.getVariableTypesBindingsStack().set(
                    (*coNames)[j], ObjectTypeSet::all());
              }
            }

            auto bodyResult =
                cg.codegen(methodNode.body(), ObjectTypeSet::all());
            builder.CreateRet(cg.getValueEncoder().box(bodyResult).value);

            for (auto &BB : *F) {
              if (!BB.getTerminator()) {
                builder.SetInsertPoint(&BB);
                builder.CreateUnreachable();
              }
            }

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

  // Register class in compiler state
  compilerState.classRegistry.registerObject(className.c_str(), cls);

  // Create Deftype instance with closed-over values as fields
  Value *classPtr = ConstantExpr::getIntToPtr(
      ConstantInt::get(types.i64Ty, reinterpret_cast<uint64_t>(cls)),
      types.ptrTy);

  vector<Type *> createParamTypes = {types.ptrTy, types.i64Ty};
  for (int i = 0; i < closedOverCount; i++)
    createParamTypes.push_back(types.RT_valueTy);
  FunctionType *createFT =
      FunctionType::get(types.ptrTy, createParamTypes, true);

  vector<Value *> createArgs;
  createArgs.push_back(classPtr);
  createArgs.push_back(ConstantInt::get(types.i64Ty, closedOverCount));
  for (auto *co : closedOverValues)
    createArgs.push_back(co);

  Value *instance =
      invokeManager.invokeRaw("Deftype_create", createFT, createArgs);

  return TypedValue(ObjectTypeSet(deftypeType), instance);
}

ObjectTypeSet CodeGen::getType(const Node &node, const ReifyNode &subnode,
                               const ObjectTypeSet &typeRestrictions) {
  return ObjectTypeSet(deftypeType);
}

} // namespace rt
