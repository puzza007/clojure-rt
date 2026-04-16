#ifndef RT_CORE_LIBRARY_H
#define RT_CORE_LIBRARY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "RTValue.h"

/* Core function implementations matching the Clojure calling convention:
   RTValue fn(arg0, arg1, ..., closureRTValue) */

RTValue core_str_0(RTValue closure);
RTValue core_str_1(RTValue a, RTValue closure);
RTValue core_str_2(RTValue a, RTValue b, RTValue closure);
RTValue core_str_3(RTValue a, RTValue b, RTValue c, RTValue closure);

RTValue core_println_0(RTValue closure);
RTValue core_println_1(RTValue a, RTValue closure);
RTValue core_println_2(RTValue a, RTValue b, RTValue closure);
RTValue core_println_3(RTValue a, RTValue b, RTValue c, RTValue closure);

RTValue core_atom_1(RTValue init, RTValue closure);
RTValue core_deref_1(RTValue ref, RTValue closure);
RTValue core_reset_BANG_2(RTValue atomVal, RTValue newVal, RTValue closure);
RTValue core_swap_BANG_2(RTValue atomVal, RTValue fn, RTValue closure);
RTValue core_swap_BANG_3(RTValue atomVal, RTValue fn, RTValue x, RTValue closure);

RTValue core_inc_1(RTValue a, RTValue closure);
RTValue core_dec_1(RTValue a, RTValue closure);

RTValue core_seq_1(RTValue coll, RTValue closure);

RTValue core_list_0(RTValue closure);
RTValue core_list_1(RTValue a, RTValue closure);
RTValue core_list_2(RTValue a, RTValue b, RTValue closure);
RTValue core_list_3(RTValue a, RTValue b, RTValue c, RTValue closure);

/* Static method implementations */
bool Numbers_isZero(RTValue a);
RTValue Numbers_inc(RTValue a);
RTValue Numbers_dec(RTValue a);
RTValue RT_count(RTValue coll);

#ifdef __cplusplus
}
#endif

#endif
