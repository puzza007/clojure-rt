#ifndef RT_PERSISTENT_HASH_SET
#define RT_PERSISTENT_HASH_SET

#ifdef __cplusplus
extern "C" {
#endif

#include "RTValue.h"
#include "String.h"

typedef struct Object Object;
typedef struct PersistentHashSet PersistentHashSet;

struct PersistentHashSet {
  Object super;
  uword_t count;
  RTValue items[HASHTABLE_THRESHOLD];
};

PersistentHashSet *PersistentHashSet_empty();
void PersistentHashSet_initialise();
void PersistentHashSet_cleanup();

bool PersistentHashSet_equals(PersistentHashSet *self,
                              PersistentHashSet *other);
uword_t PersistentHashSet_hash(PersistentHashSet *self);
String *PersistentHashSet_toString(PersistentHashSet *self);
void PersistentHashSet_destroy(PersistentHashSet *self,
                               bool deallocateChildren);
void PersistentHashSet_promoteToShared(PersistentHashSet *self, uword_t current);

PersistentHashSet *PersistentHashSet_create();
PersistentHashSet *PersistentHashSet_createMany(int32_t count, ...);

PersistentHashSet *PersistentHashSet_conj(PersistentHashSet *self, RTValue item);
PersistentHashSet *PersistentHashSet_disj(PersistentHashSet *self, RTValue item);
bool PersistentHashSet_contains(PersistentHashSet *self, RTValue item);
RTValue PersistentHashSet_get(PersistentHashSet *self, RTValue key);
RTValue PersistentHashSet_dynamic_get(RTValue self, RTValue key);

#ifdef __cplusplus
}
#endif

#endif
