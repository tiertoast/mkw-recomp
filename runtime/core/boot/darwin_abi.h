// Darwin module ABI v1. libmkw-host.1.dylib owns the module-facing symbols; the executable supplies their bodies.
// The immutable table belongs to the executable for the process lifetime and is installed before dlopen.
#pragma once
#include "cpu.h"

typedef struct {
    __typeof__(rt_dispatch) *dispatch;
    __typeof__(rt_sj_owner) *sj_owner;
    __typeof__(rt_svc) *svc;
    __typeof__(rt_udf) *udf;
    __typeof__(rt_ticks) *ticks;
    __typeof__(rt_spin_gate) *spin_gate;
    __typeof__(fnprof_hit) *prof_hit;
} RtDarwinHostV1;

__attribute__((visibility("default"))) int mkw_bind_host_v1(const RtDarwinHostV1 *host);
