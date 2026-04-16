#include "CoreLibrary.h"
#include "Object.h"
#include "Atom.h"
#include "Var.h"
#include "Function.h"
#include "PersistentList.h"
#include "PersistentArrayMap.h"
#include "PersistentHashSet.h"
#include "Deftype.h"
#include "ConcurrentHashMap.h"

extern ConcurrentHashMap *keywordsInverted;
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

/* ---- keyword invoke on deftype ---- */

RTValue core_keyword_invoke(RTValue target, RTValue keyword) {
  objectType t = getType(target);
  if (t == persistentArrayMapType) {
    return PersistentArrayMap_get((PersistentArrayMap *)RT_unboxPtr(target),
                                 keyword);
  }
  if (t == deftypeType) {
    Deftype *dt = (Deftype *)RT_unboxPtr(target);
    /* Resolve keyword name from intern table */
    RTValue nameVal =
        ConcurrentHashMap_get(keywordsInverted, keyword);
    if (RT_isNil(nameVal)) {
      Ptr_release(dt);
      return RT_boxNil();
    }
    String *nameStr = (String *)RT_unboxPtr(nameVal);
    nameStr = String_compactify(nameStr);
    RTValue result = Deftype_getFieldByName(dt, String_c_str(nameStr));
    Ptr_release(nameStr);
    Ptr_release(dt);
    return result;
  }
  if (t == persistentHashSetType) {
    return PersistentHashSet_get((PersistentHashSet *)RT_unboxPtr(target),
                                keyword);
  }
  release(target);
  release(keyword);
  return RT_boxNil();
}

/* ---- atom/deref/swap!/reset! ---- */

RTValue core_atom_1(RTValue init, RTValue closure) {
  release(closure);
  return RT_boxPtr(Atom_create(init));
}

RTValue core_deref_1(RTValue ref, RTValue closure) {
  release(closure);
  objectType t = getType(ref);
  if (t == atomType) {
    return Atom_deref((Atom *)RT_unboxPtr(ref));
  }
  if (t == varType) {
    return Var_deref((Var *)RT_unboxPtr(ref));
  }
  release(ref);
  throwIllegalArgumentException_C("deref not supported on this type");
  return RT_boxNil();
}

RTValue core_reset_BANG_2(RTValue atomVal, RTValue newVal, RTValue closure) {
  release(closure);
  if (getType(atomVal) != atomType) {
    release(atomVal);
    release(newVal);
    throwIllegalArgumentException_C("reset! requires an atom");
  }
  return Atom_reset((Atom *)RT_unboxPtr(atomVal), newVal);
}

RTValue core_swap_BANG_2(RTValue atomVal, RTValue fn, RTValue closure) {
  release(closure);
  if (getType(atomVal) != atomType) {
    release(atomVal); release(fn);
    throwIllegalArgumentException_C("swap! requires an atom");
  }
  Atom *atom = (Atom *)RT_unboxPtr(atomVal);
  ClojureFunction *func = (ClojureFunction *)RT_unboxPtr(fn);
  typedef RTValue (*Fn1)(RTValue, RTValue);
  Fn1 impl = (Fn1)Function_getBaselineImpl(func, 1);
  if (!impl) {
    Ptr_release(atom); release(fn);
    throwIllegalArgumentException_C("swap! function must accept 1 argument");
  }
  /* Simple swap — no CAS loop needed for single-threaded use */
  RTValue oldVal = atomic_load_explicit(&atom->value, memory_order_acquire);
  retain(oldVal);
  retain(fn);
  RTValue newVal = impl(oldVal, fn);
  promoteToShared(newVal);
  RTValue prev = atomic_exchange_explicit(&atom->value, newVal, memory_order_acq_rel);
  release(prev);
  retain(newVal);
  Ptr_release(atom);
  release(fn);
  return newVal;
}

RTValue core_swap_BANG_3(RTValue atomVal, RTValue fn, RTValue x, RTValue closure) {
  release(closure);
  if (getType(atomVal) != atomType) {
    release(atomVal); release(fn); release(x);
    throwIllegalArgumentException_C("swap! requires an atom");
  }
  Atom *atom = (Atom *)RT_unboxPtr(atomVal);
  ClojureFunction *func = (ClojureFunction *)RT_unboxPtr(fn);
  typedef RTValue (*Fn2)(RTValue, RTValue, RTValue);
  Fn2 impl = (Fn2)Function_getBaselineImpl(func, 2);
  if (!impl) {
    Ptr_release(atom); release(fn); release(x);
    throwIllegalArgumentException_C("swap! function must accept 2 arguments");
  }
  RTValue oldVal = atomic_load_explicit(&atom->value, memory_order_acquire);
  retain(oldVal);
  retain(fn);
  RTValue newVal = impl(oldVal, x, fn);
  promoteToShared(newVal);
  RTValue prev = atomic_exchange_explicit(&atom->value, newVal, memory_order_acq_rel);
  release(prev);
  retain(newVal);
  Ptr_release(atom);
  release(fn);
  return newVal;
}

/* ---- seq ---- */

RTValue core_seq_1(RTValue coll, RTValue closure) {
  release(closure);
  if (RT_isNil(coll))
    return RT_boxNil();
  objectType t = getType(coll);
  if (t == persistentListType) {
    PersistentList *l = (PersistentList *)RT_unboxPtr(coll);
    if (l->count == 0) {
      Ptr_release(l);
      return RT_boxNil();
    }
    return coll;
  }
  /* For other seqable types (including reify), return as-is for now */
  return coll;
}

/* ---- list ---- */

RTValue core_list_0(RTValue closure) {
  release(closure);
  return RT_boxPtr(PersistentList_createMany(0));
}

RTValue core_list_1(RTValue a, RTValue closure) {
  release(closure);
  return RT_boxPtr(PersistentList_createMany(1, a));
}

RTValue core_list_2(RTValue a, RTValue b, RTValue closure) {
  release(closure);
  return RT_boxPtr(PersistentList_createMany(2, a, b));
}

RTValue core_list_3(RTValue a, RTValue b, RTValue c, RTValue closure) {
  release(closure);
  return RT_boxPtr(PersistentList_createMany(3, a, b, c));
}

/* ---- inc/dec as Clojure calling convention wrappers ---- */

RTValue core_inc_1(RTValue a, RTValue closure) {
  release(closure);
  return Numbers_inc(a);
}

RTValue core_dec_1(RTValue a, RTValue closure) {
  release(closure);
  return Numbers_dec(a);
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
