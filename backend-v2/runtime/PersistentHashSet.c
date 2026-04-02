#include "PersistentHashSet.h"
#include "Exceptions.h"
#include "Object.h"
#include "RTValue.h"
#include "defines.h"
#include <stdarg.h>

static PersistentHashSet *EMPTY = NULL;

PersistentHashSet *PersistentHashSet_empty() {
  Ptr_retain(EMPTY);
  return EMPTY;
}

void PersistentHashSet_initialise() {
  if (EMPTY)
    return;
  EMPTY = PersistentHashSet_create();
}

void PersistentHashSet_cleanup() {
  if (EMPTY) {
    Ptr_release(EMPTY);
    EMPTY = NULL;
  }
}

PersistentHashSet *PersistentHashSet_create() {
  PersistentHashSet *self = allocate(sizeof(PersistentHashSet));
  self->count = 0;
  Object_create((Object *)self, persistentHashSetType);
  return self;
}

/* outside refcount system */
static PersistentHashSet *PersistentHashSet_copy(PersistentHashSet *other) {
  PersistentHashSet *self = allocate(sizeof(PersistentHashSet));
  Object_create((Object *)self, persistentHashSetType);
  self->count = other->count;
  memcpy(self->items, other->items, sizeof(RTValue) * self->count);
  return self;
}

/* Consumes self and item refs (matches PersistentArrayMap_indexOf convention) */
static word_t PersistentHashSet_indexOf(PersistentHashSet *self, RTValue item) {
  word_t retVal = -1;
  for (uword_t i = 0; i < self->count; i++) {
    if (equals(item, self->items[i])) {
      retVal = (word_t)i;
      break;
    }
  }
  Ptr_release(self);
  release(item);
  return retVal;
}

PersistentHashSet *PersistentHashSet_createMany(int32_t count, ...) {
  if (count > HASHTABLE_THRESHOLD) {
    char buf[64];
    snprintf(buf, sizeof(buf), "Sets of size > %d not supported yet",
             HASHTABLE_THRESHOLD);
    throwUnsupportedOperationException_C(buf);
  }
  va_list args;
  va_start(args, count);

  PersistentHashSet *self = PersistentHashSet_create();

  for (int32_t i = 0; i < count; i++) {
    RTValue item = va_arg(args, RTValue);
    bool found = false;
    for (uword_t j = 0; j < self->count; j++) {
      if (equals(item, self->items[j])) {
        found = true;
        release(item);
        break;
      }
    }
    if (!found) {
      self->items[self->count] = item;
      self->count++;
    }
  }
  va_end(args);
  return self;
}

/* outside refcount system */
bool PersistentHashSet_equals(PersistentHashSet *self,
                              PersistentHashSet *other) {
  if (self->count != other->count)
    return false;
  for (uword_t i = 0; i < self->count; i++) {
    bool found = false;
    for (uword_t j = 0; j < other->count; j++) {
      if (equals(self->items[i], other->items[j])) {
        found = true;
        break;
      }
    }
    if (!found)
      return false;
  }
  return true;
}

/* outside refcount system */
uword_t PersistentHashSet_hash(PersistentHashSet *self) {
  uword_t h = 5381;
  for (uword_t i = 0; i < self->count; i++) {
    h += hash(self->items[i]);
  }
  return h;
}

String *PersistentHashSet_toString(PersistentHashSet *self) {
  String *retVal = String_create("#{");
  String *space = String_create(" ");
  String *closing = String_create("}");

  for (uword_t i = 0; i < self->count; i++) {
    if (i > 0) {
      Ptr_retain(space);
      retVal = String_concat(retVal, space);
    }
    retain(self->items[i]);
    String *s = toString(self->items[i]);
    retVal = String_concat(retVal, s);
  }

  retVal = String_concat(retVal, closing);
  Ptr_release(space);
  Ptr_release(self);
  return retVal;
}

/* outside refcount system */
void PersistentHashSet_destroy(PersistentHashSet *self,
                               bool deallocateChildren) {
  if (deallocateChildren) {
    for (uword_t i = 0; i < self->count; i++) {
      release(self->items[i]);
    }
  }
}

void PersistentHashSet_promoteToShared(PersistentHashSet *self,
                                       uword_t current) {
  if (current & SHARED_BIT)
    return;

  for (uword_t i = 0; i < self->count; i++) {
    promoteToShared(self->items[i]);
  }
  Object_promoteToSharedShallow((Object *)self, current);
}

/* outside refcount system */
static void PersistentHashSet_retainChildren(PersistentHashSet *self,
                                             int except) {
  for (uword_t i = 0; i < self->count; i++) {
    if (except != (int)i) {
      retain(self->items[i]);
    }
  }
}

PersistentHashSet *PersistentHashSet_conj(PersistentHashSet *self,
                                          RTValue item) {
  Ptr_retain(self);
  retain(item);
  word_t found = PersistentHashSet_indexOf(self, item);
  if (found != -1) {
    release(item);
    return self;
  }
  if (self->count >= HASHTABLE_THRESHOLD) {
    Ptr_release(self);
    release(item);
    char buf[64];
    snprintf(buf, sizeof(buf), "Sets of size > %d not supported yet",
             HASHTABLE_THRESHOLD);
    throwUnsupportedOperationException_C(buf);
  }
  bool reusable = Ptr_isReusable(self);
  PersistentHashSet *retVal =
      reusable ? self : PersistentHashSet_copy(self);
  retVal->items[retVal->count] = item;
  retVal->count++;
  if (!reusable) {
    PersistentHashSet_retainChildren(retVal, retVal->count - 1);
    Ptr_release(self);
  }
  return retVal;
}

PersistentHashSet *PersistentHashSet_disj(PersistentHashSet *self,
                                          RTValue item) {
  Ptr_retain(self);
  retain(item);
  word_t found = PersistentHashSet_indexOf(self, item);
  release(item);
  if (found == -1) {
    return self;
  }
  bool reusable = Ptr_isReusable(self);
  PersistentHashSet *retVal =
      reusable ? self : PersistentHashSet_copy(self);
  RTValue removed = retVal->items[found];
  for (uword_t i = found; i < retVal->count - 1; i++) {
    retVal->items[i] = retVal->items[i + 1];
  }
  retVal->count--;
  if (!reusable) {
    PersistentHashSet_retainChildren(retVal, -1);
    Ptr_release(self);
  } else {
    release(removed);
  }
  return retVal;
}

bool PersistentHashSet_contains(PersistentHashSet *self, RTValue item) {
  Ptr_retain(self);
  retain(item);
  return PersistentHashSet_indexOf(self, item) != -1;
}

/* Set-as-function: returns the item if present, nil otherwise */
RTValue PersistentHashSet_get(PersistentHashSet *self, RTValue key) {
  for (uword_t i = 0; i < self->count; i++) {
    if (equals(key, self->items[i])) {
      RTValue retVal = self->items[i];
      retain(retVal);
      Ptr_release(self);
      release(key);
      return retVal;
    }
  }
  Ptr_release(self);
  release(key);
  return RT_boxNil();
}

RTValue PersistentHashSet_dynamic_get(RTValue self, RTValue key) {
  if (getType(self) != persistentHashSetType) {
    release(self);
    release(key);
    throwIllegalArgumentException_C("Argument must be a PersistentHashSet");
  }
  return PersistentHashSet_get(RT_unboxPtr(self), key);
}
