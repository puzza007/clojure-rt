#include "CoreLibrary.h"
#include "Object.h"
#include "Exceptions.h"
#include <stdio.h>
#include <limits.h>

/* ---- str helpers ---- */

static String *str_one(RTValue v) {
  if (RT_isNil(v))
    return String_create("");
  retain(v);
  String *s = toString(v);
  release(v);
  return s;
}

/* ---- str ---- */

RTValue core_str_0(RTValue closure) {
  release(closure);
  return RT_boxPtr(String_create(""));
}

RTValue core_str_1(RTValue a, RTValue closure) {
  release(closure);
  return RT_boxPtr(str_one(a));
}

RTValue core_str_2(RTValue a, RTValue b, RTValue closure) {
  release(closure);
  String *sa = str_one(a);
  String *sb = str_one(b);
  return RT_boxPtr(String_concat(sa, sb));
}

RTValue core_str_3(RTValue a, RTValue b, RTValue c, RTValue closure) {
  release(closure);
  String *sa = str_one(a);
  String *sb = str_one(b);
  String *sc = str_one(c);
  sa = String_concat(sa, sb);
  return RT_boxPtr(String_concat(sa, sc));
}

/* ---- println ---- */

static void print_value(RTValue val) {
  retain(val);
  String *s = toString(val);
  s = String_compactify(s);
  printf("%s", String_c_str(s));
  Ptr_release(s);
}

RTValue core_println_0(RTValue closure) {
  release(closure);
  printf("\n");
  return RT_boxNil();
}

RTValue core_println_1(RTValue a, RTValue closure) {
  release(closure);
  print_value(a);
  release(a);
  printf("\n");
  return RT_boxNil();
}

RTValue core_println_2(RTValue a, RTValue b, RTValue closure) {
  release(closure);
  print_value(a); release(a);
  printf(" ");
  print_value(b); release(b);
  printf("\n");
  return RT_boxNil();
}

RTValue core_println_3(RTValue a, RTValue b, RTValue c, RTValue closure) {
  release(closure);
  print_value(a); release(a);
  printf(" ");
  print_value(b); release(b);
  printf(" ");
  print_value(c); release(c);
  printf("\n");
  return RT_boxNil();
}

/* ---- Numbers static methods ---- */

bool Numbers_isZero(RTValue a) {
  objectType t = getType(a);
  if (t == integerType)
    return RT_unboxInt32(a) == 0;
  if (t == doubleType)
    return RT_unboxDouble(a) == 0.0;
  if (t == bigIntegerType) {
    BigInteger *b = (BigInteger *)RT_unboxPtr(a);
    bool result = mpz_sgn(b->value) == 0;
    Ptr_release(b);
    return result;
  }
  if (t == ratioType) {
    Ratio *r = (Ratio *)RT_unboxPtr(a);
    bool result = mpq_sgn(r->value) == 0;
    Ptr_release(r);
    return result;
  }
  release(a);
  throwIllegalArgumentException_C("isZero requires a numeric argument");
  return false;
}

RTValue Numbers_inc(RTValue a) {
  objectType t = getType(a);
  if (t == integerType) {
    int32_t val = RT_unboxInt32(a);
    if (val == INT32_MAX) {
      BigInteger *b = BigInteger_createFromInt(val);
      BigInteger *one = BigInteger_createFromInt(1);
      return RT_boxPtr(BigInteger_add(b, one));
    }
    return RT_boxInt32(val + 1);
  }
  if (t == doubleType)
    return RT_boxDouble(RT_unboxDouble(a) + 1.0);
  if (t == bigIntegerType) {
    BigInteger *b = (BigInteger *)RT_unboxPtr(a);
    BigInteger *one = BigInteger_createFromInt(1);
    Ptr_retain(b);
    return RT_boxPtr(BigInteger_add(b, one));
  }
  release(a);
  throwIllegalArgumentException_C("inc requires a numeric argument");
  return RT_boxNil();
}

RTValue Numbers_dec(RTValue a) {
  objectType t = getType(a);
  if (t == integerType) {
    int32_t val = RT_unboxInt32(a);
    if (val == INT32_MIN) {
      BigInteger *b = BigInteger_createFromInt(val);
      BigInteger *one = BigInteger_createFromInt(1);
      return RT_boxPtr(BigInteger_sub(b, one));
    }
    return RT_boxInt32(val - 1);
  }
  if (t == doubleType)
    return RT_boxDouble(RT_unboxDouble(a) - 1.0);
  if (t == bigIntegerType) {
    BigInteger *b = (BigInteger *)RT_unboxPtr(a);
    BigInteger *one = BigInteger_createFromInt(1);
    Ptr_retain(b);
    return RT_boxPtr(BigInteger_sub(b, one));
  }
  release(a);
  throwIllegalArgumentException_C("dec requires a numeric argument");
  return RT_boxNil();
}

/* ---- RT/count ---- */

RTValue RT_count(RTValue coll) {
  objectType t = getType(coll);
  switch (t) {
  case persistentVectorType: {
    PersistentVector *v = (PersistentVector *)RT_unboxPtr(coll);
    uword_t count = PersistentVector_count(v);
    Ptr_release(v);
    return RT_boxInt32((int32_t)count);
  }
  case persistentListType: {
    PersistentList *l = (PersistentList *)RT_unboxPtr(coll);
    uword_t count = l->count;
    Ptr_release(l);
    return RT_boxInt32((int32_t)count);
  }
  case persistentArrayMapType: {
    PersistentArrayMap *m = (PersistentArrayMap *)RT_unboxPtr(coll);
    uword_t count = m->count;
    Ptr_release(m);
    return RT_boxInt32((int32_t)count);
  }
  case persistentHashSetType: {
    PersistentHashSet *s = (PersistentHashSet *)RT_unboxPtr(coll);
    uword_t count = s->count;
    Ptr_release(s);
    return RT_boxInt32((int32_t)count);
  }
  case stringType: {
    String *s = (String *)RT_unboxPtr(coll);
    uword_t count = s->count;
    Ptr_release(s);
    return RT_boxInt32((int32_t)count);
  }
  case nilType:
    return RT_boxInt32(0);
  default:
    release(coll);
    throwIllegalArgumentException_C("count not supported on this type");
    return RT_boxInt32(0);
  }
}
