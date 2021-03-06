#ifndef __SCHEDULER_H__
#define __SCHEDULER_H__

#include "armv7_p15.h"
#include "context.h"
#include "uart_print.h"
#include "timer.h"

hvmm_status_t scheduler_next_event(int irq, void *pdata);

/* Test Code */
void scheduler_test_scheduling(void);

/* Schedules guest context switch according to the default scheduling policy (sched_policy.c) */
void scheduler_schedule(void);
#endif
