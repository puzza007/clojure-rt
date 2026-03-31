#ifndef RT_DEFTYPE
#define RT_DEFTYPE

#ifdef __cplusplus
extern "C" {
#endif

#include "Class.h"
#include "RTValue.h"
#include "String.h"
#include "defines.h"

struct Deftype {
  Object super;
  Class *_class;
  uword_t fieldCount;
  RTValue values[]; // NaN-boxed field values, indexed by field position
};

typedef struct Deftype Deftype;

Deftype *Deftype_create(Class *_class, uword_t fieldCount, ...);
bool Deftype_equals(Deftype *self, Deftype *other);
uword_t Deftype_hash(Deftype *self);
String *Deftype_toString(Deftype *self);
void Deftype_destroy(Deftype *self);
RTValue Deftype_getIndexedField(Deftype *self, uword_t index);
RTValue Deftype_getFieldByName(Deftype *self, const char *fieldName);
void Deftype_setFieldByName(Deftype *self, const char *fieldName, RTValue value);

#ifdef __cplusplus
}
#endif

#endif
