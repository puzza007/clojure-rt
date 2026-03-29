// Java .hashCode() equivalents for case dispatch
// Matches what Clojure's case macro stores in the protobuf

#include "Hasheq.h"
#include "ConcurrentHashMap.h"
#include "Object.h"
#include "RTValue.h"
#include "String.h"
#include <stdint.h>
#include <string.h>

extern ConcurrentHashMap *keywordsInverted;

// Java String.hashCode: h = 31*h + c for each UTF-16 code unit.
// For ASCII strings, UTF-16 code units = byte values.
int32_t javaStringHashCode(const char *s) {
  int32_t h = 0;
  while (*s) {
    h = 31 * h + (int32_t)(unsigned char)*s;
    s++;
  }
  return h;
}

// Clojure's Util.hashCombine
static int32_t hashCombine(int32_t seed, int32_t hash) {
  return seed ^ (hash + (int32_t)0x9e3779b9 + (seed << 6) +
                 (int32_t)((uint32_t)seed >> 2));
}

int32_t javaHashCode(RTValue v) {
  if (RT_isInt32(v)) {
    return RT_unboxInt32(v);
  }
  if (RT_isKeyword(v)) {
    // Keyword.hashCode = Symbol.hashCode(name) + 0x9e3779b9
    // Symbol.hashCode = hashCombine(name.hashCode(), 0) for unnamespaced
    RTValue nameVal = ConcurrentHashMap_get(keywordsInverted, v);
    if (!RT_isNil(nameVal)) {
      // ConcurrentHashMap_get consumes a ref on the key and retains the value.
      // nameVal is a boxed String* with one retained ref we must release.
      String *nameStr = (String *)RT_unboxPtr(nameVal);
      int32_t nameHash = javaStringHashCode(String_c_str(nameStr));
      int32_t symHash = hashCombine(nameHash, 0);
      release(nameVal);
      return symHash + (int32_t)0x9e3779b9;
    }
    return 0;
  }
  if (RT_isPtr(v)) {
    Object *obj = (Object *)RT_unboxPtr(v);
    if (obj->type == stringType) {
      return javaStringHashCode(String_c_str((String *)obj));
    }
  }
  return (int32_t)hash(v);
}
