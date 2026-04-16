#ifndef RT_ATOM_H
#define RT_ATOM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "RTValue.h"
#include "String.h"

typedef struct Object Object;
typedef struct Atom Atom;

struct Atom {
  Object super;
  _Atomic(RTValue) value;
};

Atom *Atom_create(RTValue initialValue);
RTValue Atom_deref(Atom *self);
RTValue Atom_reset(Atom *self, RTValue newVal);

bool Atom_equals(Atom *self, Atom *other);
uword_t Atom_hash(Atom *self);
String *Atom_toString(Atom *self);
void Atom_destroy(Atom *self, bool deallocateChildren);
void Atom_promoteToShared(Atom *self, uword_t current);

#ifdef __cplusplus
}
#endif

#endif
