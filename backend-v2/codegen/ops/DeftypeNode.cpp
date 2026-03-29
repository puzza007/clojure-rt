#include "../CodeGen.h"
#include "runtime/Class.h"
#include "runtime/Deftype.h"
#include "tools/EdnParser.h"
#include "tools/RTValueWrapper.h"

using namespace std;
using namespace llvm;
using namespace clojure::rt::protobuf::bytecode;

namespace rt {

// Runtime helper declared here, implemented in bridge/Exceptions.cpp or a new file
extern "C" Class *Deftype_registerClass(const char *name, const char *className,
                                        const char **fieldNames,
                                        int32_t fieldCount, void *state);

TypedValue CodeGen::codegen(const Node &node, const DeftypeNode &subnode,
                            const ObjectTypeSet &typeRestrictions) {
  // Extract field names
  int fieldCount = subnode.fields_size();
  vector<string> fieldNames;
  for (int i = 0; i < fieldCount; i++) {
    fieldNames.push_back(subnode.fields(i).subnode().binding().name());
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

  // Register in compiler state
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
