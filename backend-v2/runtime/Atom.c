#include "Atom.h"
#include "Object.h"

Atom *Atom_create(RTValue initialValue) {
  Atom *self = allocate(sizeof(Atom));
  Object_create((Object *)self, atomType);
  // Atom is always shared (accessed from multiple JIT compilations)
  atomic_store_explicit(&((Object *)self)->atomicRefCount,
                        COUNT_INC | SHARED_BIT, memory_order_release);
  promoteToShared(initialValue);
  atomic_store_explicit(&self->value, initialValue, memory_order_release);
  return self;
}

RTValue Atom_deref(Atom *self) {
  RTValue val = atomic_load_explicit(&self->value, memory_order_acquire);
  retain(val);
  Ptr_release(self);
  return val;
}

RTValue Atom_reset(Atom *self, RTValue newVal) {
  promoteToShared(newVal);
  RTValue oldVal =
      atomic_exchange_explicit(&self->value, newVal, memory_order_acq_rel);
  release(oldVal);
  retain(newVal);
  Ptr_release(self);
  return newVal;
}

bool Atom_equals(Atom *self, Atom *other) {
  return self == other;
}

uword_t Atom_hash(Atom *self) {
  return (uword_t)(uintptr_t)self;
}

String *Atom_toString(Atom *self) {
  RTValue val = atomic_load_explicit(&self->value, memory_order_acquire);
  retain(val);
  String *inner = toString(val);
  release(val);
  String *prefix = String_create("atom@");
  String *result = String_concat(prefix, inner);
  Ptr_release(self);
  return result;
}

void Atom_destroy(Atom *self, bool deallocateChildren) {
  if (deallocateChildren) {
    RTValue val = atomic_load_explicit(&self->value, memory_order_acquire);
    release(val);
  }
}

void Atom_promoteToShared(Atom *self, uword_t current) {
  if (current & SHARED_BIT)
    return;
  RTValue val = atomic_load_explicit(&self->value, memory_order_acquire);
  promoteToShared(val);
  Object_promoteToSharedShallow((Object *)self, current);
}
