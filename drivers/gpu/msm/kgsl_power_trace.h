/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022, 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#if !defined(_KGSL_POWER_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _KGSL_POWER_TRACE_H

#undef TRACE_SYSTEM
#define TRACE_SYSTEM power
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE kgsl_power_trace

#include <linux/tracepoint.h>
 /*
 * Tracepoint for power gpu_frequency
 */
  TRACE_EVENT(gpu_frequency,
	TP_PROTO(u32 state, u32 gpu_id),
	TP_ARGS(state, gpu_id),
	TP_STRUCT__entry(
		__field(unsigned int, state)
		__field(unsigned int, gpu_id)
	),
	TP_fast_assign(
		__entry->state = state;
		__entry->gpu_id = gpu_id;
	),

	TP_printk("state=%lu gpu_id=%lu",
		(unsigned long)__entry->state,
		(unsigned long)__entry->gpu_id)
 );
#endif /* _KGSL_POWER_TRACE_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
