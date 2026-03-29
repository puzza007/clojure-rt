#ifndef RT_JAVA_HASHCODE
#define RT_JAVA_HASHCODE

#include "RTValue.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Java-compatible String.hashCode (ASCII only; UTF-8 multi-byte not handled)
int32_t javaStringHashCode(const char *s);

// Compute Java .hashCode() equivalent for an RTValue.
// Matches what Clojure's case macro stores in the protobuf.
int32_t javaHashCode(RTValue v);

#ifdef __cplusplus
}
#endif

#endif
