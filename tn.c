/*

    SATNKernel real-time kernel for the Sega Saturn
    Based on TNKernel version 2.7

    Copyright © 2004, 2013 Yuri Tiomkin
    Saturn version modifications copyright © 2013 Anders Montonen
    All rights reserved.

    Permission to use, copy, modify, and distribute this software in source
    and binary forms and its documentation for any purpose and without fee
    is hereby granted, provided that the above copyright notice appear
    in all copies and that both that copyright notice and this permission
    notice appear in supporting documentation.

    THIS SOFTWARE IS PROVIDED BY THE YURI TIOMKIN AND CONTRIBUTORS "AS IS" AND
    ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED. IN NO EVENT SHALL YURI TIOMKIN OR CONTRIBUTORS BE LIABLE
    FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
    DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
    OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
    HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
    OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
    SUCH DAMAGE.

 */

#include "tn.h"
#include "tn_utils.h"
#include "tn_port_config.h"

//  The System uses two levels of priorities for the own purpose:
//
//   - level 0                    (highest) for system timers task
//   - level (TN_NUM_PRIORITY-1)  (lowest)  for system idle task

//-- System tasks

//-- timer task - priority 0  - highest

static void tn_timer_task_func(void * par);

//-- idle task - priority (TN_NUM_PRIORITY-1) - lowest

static void tn_idle_task_func(void * par);


//----------------------------------------------------------------------------
// TN main function (never return)
//----------------------------------------------------------------------------
void tn_start_system(TN_KERN_CTX *kctx)
{
    int i;

    //-- Hook vectors
    tn_hook_vec(TN_KERNEL_VECTOR, kctx);
    tn_hook_vec(TN_CONTEXT_SWITCH_TRAP, tn_switch_context_trap);

    //-- Clear/set all globals (vars, lists, etc)

    for (i = 0; i < TN_NUM_PRIORITY; i++)
    {
        queue_reset(&(kctx->tn_ready_list[i]));
        kctx->tn_tslice_ticks[i] = NO_TIME_SLICE;
    }

    queue_reset(&kctx->tn_create_queue);
    kctx->tn_created_tasks_qty = 0;

    kctx->tn_system_state = TN_ST_STATE_NOT_RUN;

    kctx->tn_int_nest_count = 0;

    kctx->tn_ready_to_run_bmp = 0;

    kctx->tn_idle_count       = 0;
    kctx->tn_curr_performance = 0;

    kctx->tn_next_task_to_run = NULL;
    kctx->tn_curr_run_task    = NULL;

#ifdef TN_INT_STACK
    // SH uses pre-decrement stack pointer
    kctx->tn_int_sp = &kctx->tn_int_stack[TN_INT_STACK_SIZE];
#endif

    //-- System tasks

    queue_reset(&kctx->tn_wait_timeout_list);

    //--- Timer task

    tn_task_create(&kctx->tn_timer_task,           //-- task TCB
                   tn_timer_task_func,             //-- task function
                   0,                              //-- task priority
                   &(kctx->tn_timer_task_stack     //-- task stack first addr in memory
                     [TN_TIMER_STACK_SIZE-1]),
                   TN_TIMER_STACK_SIZE,            //-- task stack size (in int,not bytes)
                   NULL,                           //-- task function parameter
                   TN_TASK_TIMER);                 //-- Creation option

    //--- Idle task

    tn_task_create(&kctx->tn_idle_task,            //-- task TCB
                   tn_idle_task_func,              //-- task function
                   TN_NUM_PRIORITY-1,              //-- task priority
                   &(kctx->tn_idle_task_stack      //-- task stack first addr in memory
                     [TN_IDLE_STACK_SIZE-1]),
                   TN_IDLE_STACK_SIZE,             //-- task stack size (in int,not bytes)
                   NULL,                           //-- task function parameter
                   TN_TASK_IDLE);                  //-- Creation option

    //-- Activate timer & idle tasks

    kctx->tn_next_task_to_run = &kctx->tn_idle_task; //-- Just for the task_to_runnable() proper op

    task_to_runnable(&kctx->tn_idle_task);
    task_to_runnable(&kctx->tn_timer_task);

    kctx->tn_curr_run_task = &kctx->tn_idle_task;  //-- otherwise it is NULL

    //-- Run OS - first context switch

    tn_start_exe();
}

//----------------------------------------------------------------------------
static void tn_timer_task_func(void * par)
{
    TN_INTSAVE_DATA
    volatile TN_TCB * task;
    volatile CDLL_QUEUE * curr_que;
    TN_KERN_CTX * kctx;

    //-- User application init - user's objects initial (tasks etc.) creation

    tn_app_init();

    //-- Enable interrupt here (include tick int)

    tn_cpu_int_enable();

    kctx = tn_kern_ctx_ptr();

    //-------------------------------------------------------------------------

    for (;;)
    {

        //------------ OS timer tick -------------------------------------

        tn_disable_interrupt();

        curr_que = kctx->tn_wait_timeout_list.next;
        while (curr_que != &kctx->tn_wait_timeout_list)
        {
            task = get_task_by_timer_queque((CDLL_QUEUE*)curr_que);
            if (task->tick_count != TN_WAIT_INFINITE)
            {
                if (task->tick_count > 0)
                {
                    task->tick_count--;
                    if (task->tick_count == 0) //-- Time out expiried
                    {
                        queue_remove_entry(&(((TN_TCB*)task)->task_queue));
                        task_wait_complete((TN_TCB*)task);
                        task->task_wait_rc = TERR_TIMEOUT;
                    }
                }
            }

            curr_que = curr_que->next;
        }

        task_curr_to_wait_action(NULL,
                                 TSK_WAIT_REASON_SLEEP,
                                 TN_WAIT_INFINITE);
        tn_enable_interrupt();

        tn_switch_context();
    }
}

//----------------------------------------------------------------------------
//  In fact, this task is always in RUNNABLE state
//----------------------------------------------------------------------------
static void tn_idle_task_func(void * par)
{
    TN_KERN_CTX * kctx;

#ifdef TN_MEAS_PERFORMANCE
    TN_INTSAVE_DATA
#endif

    kctx = tn_kern_ctx_ptr();

    for(;;)
    {
#ifdef TN_MEAS_PERFORMANCE
        tn_disable_interrupt();
#endif

        kctx->tn_idle_count++;

#ifdef TN_MEAS_PERFORMANCE
        tn_enable_interrupt();
#endif
    }
}

//--- Set time slice ticks value for priority for round-robin scheduling
//--- If value is NO_TIME_SLICE there are no round-robin scheduling
//--- for tasks with priority. NO_TIME_SLICE is default value.
//----------------------------------------------------------------------------
int tn_sys_tslice_ticks(int priority, int value)
{
    TN_CHECK_NON_INT_CONTEXT

    if (priority <= 0 || priority >= TN_NUM_PRIORITY-1 ||
        value < 0 || value > MAX_TIME_SLICE)
        return TERR_WRONG_PARAM;

    tn_kern_ctx_ptr()->tn_tslice_ticks[priority] = value;

    return TERR_NO_ERR;
}

//----------------------------------------------------------------------------
void  tn_tick_int_processing()
{
    TN_INTSAVE_DATA_INT

    volatile CDLL_QUEUE * curr_que;   //-- Need volatile here only to solve
    volatile CDLL_QUEUE * pri_queue;  //-- IAR(c) compiler's high optimization mode problem
    volatile int priority;

    TN_KERN_CTX * kctx;

    TN_CHECK_INT_CONTEXT_NORETVAL

    kctx = tn_kern_ctx_ptr();

    tn_idisable_interrupt();

    //-------  Round -robin (if is used)

    priority  = kctx->tn_curr_run_task->priority;

    if (kctx->tn_tslice_ticks[priority] != NO_TIME_SLICE)
    {
        kctx->tn_curr_run_task->tslice_count++;
        if (kctx->tn_curr_run_task->tslice_count > kctx->tn_tslice_ticks[priority])
        {
            kctx->tn_curr_run_task->tslice_count = 0;

            pri_queue = &(kctx->tn_ready_list[priority]);
            //-- If ready queue is not empty and qty  of queue's tasks > 1
            if (!(is_queue_empty((CDLL_QUEUE *)pri_queue)) && pri_queue->next->next != pri_queue)
            {
                // v.2.7  - Thanks to Vyacheslav Ovsiyenko

                //-- Remove task from head and add it to the tail of
                //-- ready queue for current priority

                curr_que = queue_remove_head(&(kctx->tn_ready_list[priority]));
                queue_add_tail(&(kctx->tn_ready_list[priority]), (CDLL_QUEUE *)curr_que);
            }
        }
    }

    //-- Enable a task with priority 0 - tn_timer_task

    queue_remove_entry(&(kctx->tn_timer_task.task_queue));
    kctx->tn_timer_task.task_wait_reason = 0;
    kctx->tn_timer_task.task_state       = TSK_STATE_RUNNABLE;
    kctx->tn_timer_task.pwait_queue      = NULL;
    kctx->tn_timer_task.task_wait_rc     = TERR_NO_ERR;

    queue_add_tail(&(kctx->tn_ready_list[0]), &(kctx->tn_timer_task.task_queue));
    kctx->tn_ready_to_run_bmp |= 1;  // priority 0;

    kctx->tn_next_task_to_run = &kctx->tn_timer_task;

    tn_ienable_interrupt();  //--  !!! thanks to Audrius Urmanavicius !!!
}

//----------------------------------------------------------------------------
//----------------------------------------------------------------------------
//----------------------------------------------------------------------------
//----------------------------------------------------------------------------
