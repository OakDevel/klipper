// Basic scheduling functions and startup/shutdown code.
//
// Copyright (C) 2016  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <setjmp.h> // setjmp
#include "autoconf.h" // CONFIG_*
#include "board/irq.h" // irq_save
#include "board/misc.h" // timer_from_us
#include "command.h" // shutdown
#include "sched.h" // sched_check_periodic
#include "stepper.h" // stepper_event


/****************************************************************
 * Timers
 ****************************************************************/

static uint16_t millis;

// Check if ready for a recurring periodic event
uint8_t
sched_check_periodic(uint16_t time, uint16_t *pnext)
{
    uint16_t next = *pnext, cur;
    irqstatus_t flag = irq_save();
    cur = millis;
    irq_restore(flag);
    if ((int16_t)(cur - next) < 0)
        return 0;
    *pnext = cur + time;
    return 1;
}

static struct timer ms_timer, sentinel_timer, *timer_list = &ms_timer;

// Default millisecond timer.  This timer counts milliseconds.  It
// also simplifies the timer code by ensuring there is always a timer
// on the timer list and that there is always a timer not more than
// 1ms in the future.
static uint_fast8_t
ms_event(struct timer *t)
{
    millis++;
    timer_periodic();
    ms_timer.waketime += timer_from_us(1000);
    sentinel_timer.waketime = ms_timer.waketime + 0x80000000;
    return SF_RESCHEDULE;
}

static struct timer ms_timer = {
    .func = ms_event,
    .next = &sentinel_timer,
};

// The sentinel timer is always the last timer on timer_list - its
// presence allows the code to avoid checking for NULL while
// traversing timer_list.  Since sentinel_timer.waketime is always
// equal to (ms_timer.waketime + 0x80000000) any added timer must
// always have a waketime less than one of these two timers.
static uint_fast8_t
sentinel_event(struct timer *t)
{
    shutdown("sentinel timer called");
}

static struct timer sentinel_timer = {
    .func = sentinel_event,
    .waketime = 0x80000000,
};

// Find position for a timer in timer_list and insert it
static void __always_inline
insert_timer(struct timer *t, uint32_t waketime)
{
    struct timer *prev, *pos = timer_list;
    for (;;) {
        prev = pos;
        if (CONFIG_MACH_AVR)
            // micro optimization for AVR - reduces register pressure
            asm("" : "+r"(prev));
        pos = pos->next;
        if (timer_is_before(waketime, pos->waketime))
            break;
    }
    t->next = pos;
    prev->next = t;
}

// Schedule a function call at a supplied time.
void
sched_add_timer(struct timer *add)
{
    uint32_t waketime = add->waketime;
    irqstatus_t flag = irq_save();
    if (timer_is_before(waketime, timer_list->waketime))
        // Timer in past (or very near future)
        shutdown("Timer too close");
    insert_timer(add, waketime);
    irq_restore(flag);
}

// The deleted timer is used when deleting an active timer.
static uint_fast8_t
deleted_event(struct timer *t)
{
    return SF_DONE;
}

static struct timer deleted_timer = {
    .func = deleted_event,
};

// Remove a timer that may be live.
void
sched_del_timer(struct timer *del)
{
    irqstatus_t flag = irq_save();
    if (timer_list == del) {
        // Deleting the next active timer - replace with deleted_timer
        deleted_timer.waketime = del->waketime;
        deleted_timer.next = del->next;
        timer_list = &deleted_timer;
    } else {
        // Find and remove from timer list (if present)
        struct timer *pos;
        for (pos = timer_list; pos->next; pos = pos->next) {
            if (pos->next == del) {
                pos->next = del->next;
                break;
            }
        }
    }
    irq_restore(flag);
}

// Invoke the next timer - called from board hardware irq code.
unsigned int
sched_timer_dispatch(void)
{
    // Invoke timer callback
    struct timer *t = timer_list;
    uint_fast8_t res;
    uint32_t updated_waketime;
    if (CONFIG_INLINE_STEPPER_HACK && likely(!t->func)) {
        res = stepper_event(t);
        updated_waketime = t->waketime;
    } else {
        res = t->func(t);
        updated_waketime = t->waketime;
    }

    // Update timer_list (rescheduling current timer if necessary)
    unsigned int next_waketime = updated_waketime;
    if (unlikely(res == SF_DONE)) {
        next_waketime = t->next->waketime;
        timer_list = t->next;
    } else if (!timer_is_before(updated_waketime, t->next->waketime)) {
        next_waketime = t->next->waketime;
        timer_list = t->next;
        insert_timer(t, updated_waketime);
    }

    return next_waketime;
}

// Shutdown all user timers on an emergency stop.
static void
sched_timer_shutdown(void)
{
    timer_list = &deleted_timer;
    deleted_timer.waketime = ms_timer.waketime;
    deleted_timer.next = &ms_timer;
    ms_timer.next = &sentinel_timer;
}
DECL_SHUTDOWN(sched_timer_shutdown);


/****************************************************************
 * Shutdown processing
 ****************************************************************/

static uint16_t shutdown_reason;
static uint8_t shutdown_status;

// Return true if the machine is in an emergency stop state
uint8_t
sched_is_shutdown(void)
{
    return !!shutdown_status;
}

uint16_t
sched_shutdown_reason(void)
{
    return shutdown_reason;
}

// Transition out of shutdown state
void
sched_clear_shutdown(void)
{
    if (!shutdown_status)
        shutdown("Shutdown cleared when not shutdown");
    if (shutdown_status == 2)
        // Ignore attempt to clear shutdown if still processing shutdown
        return;
    shutdown_status = 0;
}

// Invoke all shutdown functions (as declared by DECL_SHUTDOWN)
static void
run_shutdown(void)
{
    uint32_t cur = timer_read_time();
    shutdown_status = 2;
    struct callback_handler *p;
    foreachdecl(p, shutdownfuncs) {
        void (*func)(void) = READP(p->func);
        func();
    }
    shutdown_status = 1;
    irq_enable();

    extern uint8_t debug_parserid, debug_t;
    extern void *debug_p, *debug_maxend, *debug_buf;
    output("last %c %c %hu %hu %hu"
           , debug_parserid, debug_t, debug_buf, debug_p, debug_maxend);

    sendf("shutdown clock=%u static_string_id=%hu", cur, shutdown_reason);
}

// Shutdown the machine if not already in the process of shutting down
void
sched_try_shutdown(unsigned int reason)
{
    if (shutdown_status != 2)
        sched_shutdown(reason);
}

static jmp_buf shutdown_jmp;

// Force the machine to immediately run the shutdown handlers
void
sched_shutdown(unsigned int reason)
{
    irq_disable();
    if (!shutdown_status)
        shutdown_reason = reason;
    longjmp(shutdown_jmp, 1);
}


/****************************************************************
 * Startup and background task processing
 ****************************************************************/

// Invoke all init functions (as declared by DECL_INIT)
static void
run_init(void)
{
    struct callback_handler *p;
    foreachdecl(p, initfuncs) {
        void (*func)(void) = READP(p->func);
        func();
    }
}

// Invoke all background task functions (as declared by DECL_TASK)
static void
run_task(void)
{
    struct callback_handler *p;
    foreachdecl(p, taskfuncs) {
        void (*func)(void) = READP(p->func);
        func();
    }
}

// Main loop of program
void
sched_main(void)
{
    run_init();

    int ret = setjmp(shutdown_jmp);
    if (ret)
        run_shutdown();

    for (;;)
        run_task();
}
