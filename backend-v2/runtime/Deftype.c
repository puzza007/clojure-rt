#include "Deftype.h"
#include "Hash.h"
#include "Object.h"
#include "Symbol.h"
#include <stdarg.h>

Deftype *Deftype_create(Class *_class, uword_t fieldCount, ...) {
  Deftype *self =
      (Deftype *)allocate(sizeof(Deftype) + fieldCount * sizeof(RTValue));
  self->_class = _class;
  self->fieldCount = fieldCount;
  retain((RTValue)_class);
  va_list args;
  va_start(args, fieldCount);
  for (uword_t i = 0; i < fieldCount; i++) {
    self->values[i] = va_arg(args, RTValue);
  }
  va_end(args);
  Object_create((Object *)self, deftypeType);
  return self;
}

bool Deftype_equals(Deftype *self, Deftype *other) {
  return self == other;
}

uword_t Deftype_hash(Deftype *self) {
  return avalanche((uword_t)self);
}

String *Deftype_toString(Deftype *self) {
  retain((RTValue)self->_class->className);
  String *result = String_concat(self->_class->className, String_create("@"));
  char buf[20];
  snprintf(buf, sizeof(buf), "%lx", (unsigned long)self);
  result = String_concat(result, String_create(buf));
  Ptr_release(self);
  return result;
}

void Deftype_destroy(Deftype *self) {
  for (uword_t i = 0; i < self->fieldCount; i++) {
    release(self->values[i]);
  }
  release((RTValue)self->_class);
}

// Non-consuming: does not release self
RTValue Deftype_getFieldByName(Deftype *self, const char *fieldName) {
  RTValue sym = Symbol_create(String_createDynamicStr(fieldName));
  // Class_fieldIndex consumes both self and field, so retain the class first
  Ptr_retain(self->_class);
  int32_t idx = Class_fieldIndex(self->_class, sym);
  if (idx < 0) {
    return RT_boxNil();
  }
  RTValue val = self->values[idx];
  retain(val);
  return val;
}

RTValue Deftype_getIndexedField(Deftype *self, uword_t index) {
  RTValue val = self->values[index];
  retain(val);
  Ptr_release(self);
  return val;
}
