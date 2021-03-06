/*
* minithread.c:
*      This file provides a few function headers for the procedures that
*      you are required to implement for the minithread assignment.
*
*      EXCEPT WHERE NOTED YOUR IMPLEMENTATION MUST CONFORM TO THE
*      NAMING AND TYPING OF THESE PROCEDURES.
*
*/
#include <stdlib.h>
#include <stdio.h>
#include "interrupts.h"
#include "minithread.h"
#include "queue.h"
#include "multilevel_queue.h"
#include "synch.h"
#include "alarm.h"
#include "network.h"
#include "minimsg.h"
#include "minisocket.h"

#include <assert.h>

static long current_tick = 0;


/*
* A minithread should be defined either in this file or in a private
* header file.  Minithreads have a stack pointer with to make procedure
* calls, a stackbase which points to the bottom of the procedure
* call stack, the ability to be enqueueed and dequeued, and any other state
* that you feel they must have.
*/

typedef enum {READY, WAITING, RUNNING, FINISHED} state_t;


typedef struct minithread {
	int pid;
	stack_pointer_t sp;
	stack_pointer_t stackbase;
	stack_pointer_t stacktop;
	state_t state;
	int idling;
} minithread;

//Current running thread.
minithread_t current_thread = NULL;

//Semaphore for cleaning up only when needed.
semaphore_t cleanup_sema = NULL;

/*
	Scheduler definition. (Multivel additions for p2)
*/

int number_of_levels = 4;
int maxval = 100;
static int quanta_durations[4] = {1, 2, 4, 8};
static int quanta_proportions[4] = {0,50, 75, 90};

typedef struct scheduler {
	multilevel_queue_t 	ready_queue;
	queue_t 			finished_queue;
	unsigned int 		level;
	unsigned int 		quanta_count;
	unsigned int 		freq_count;
} scheduler;
typedef struct scheduler *scheduler_t;

scheduler_t thread_scheduler;

/*
	Scheduler API.

*/

void scheduler_init(scheduler_t *scheduler_ptr){
	
	scheduler_t s;

	*scheduler_ptr = (scheduler_t) malloc(sizeof(struct scheduler));
	s = *scheduler_ptr;
	s->ready_queue = multilevel_queue_new(number_of_levels);
	s->finished_queue = queue_new();
	s->level = 0;
	s->quanta_count = 0;
	s->freq_count = 0;

	cleanup_sema 		= semaphore_create();
	semaphore_initialize(cleanup_sema, 0);

}

//Pick the level to peek on the multilevel_queue based on the frequency counter.
int scheduler_pick_level(scheduler_t scheduler){

	if(scheduler->freq_count++ < quanta_proportions[1]) return 0;

	if(scheduler->freq_count   < quanta_proportions[2]) return 1;

	if(scheduler->freq_count   < quanta_proportions[3]) return 2;

	if(scheduler->freq_count == maxval) scheduler->freq_count = 0;

	return 3;
}


/*
* Try to context switch only once, return 1 (success) if it found a valid TCB to switch to, otherwise 0 (failure).
*/
int scheduler_switch_dequeue(scheduler_t scheduler){
	minithread_t thread_to_run;	

	interrupt_level_t old_level;
	int deq_level;

	//Scheduler cannot be interrupted while it's trying to dequeue.
	old_level = set_interrupt_level(DISABLED);

	scheduler->quanta_count++;

	/* 
		We have to context switch only if either the max number of quanta for the current level has expired or
	   	the current thread has finished or put to wait before it expires. 
	   	We also need to switch if we're scheduling for the first time.
	*/
	if(scheduler->quanta_count >= quanta_durations[scheduler->level] || 
		current_thread == NULL || current_thread->state == FINISHED || current_thread->state == WAITING){
		
		unsigned int old_queue_level;
		if(scheduler->level == number_of_levels - 1){
			old_queue_level = scheduler->level - 1;
		} else {
			old_queue_level = scheduler->level;
		} 

		scheduler->quanta_count = 0;
		scheduler->level = scheduler_pick_level(scheduler);

		deq_level = multilevel_queue_dequeue(scheduler->ready_queue, scheduler->level, (void **) &thread_to_run);

		if(deq_level == -1){
			//No threads found for the new level, return to 0.
			scheduler->level = 0;
			scheduler->freq_count = quanta_proportions[scheduler->level];
			deq_level = multilevel_queue_dequeue(scheduler->ready_queue, scheduler->level, (void **) &thread_to_run);
		}

		if(deq_level != -1){
			stack_pointer_t *oldsp_ptr; 

			if(deq_level != scheduler->level){
				scheduler->level = deq_level;
				scheduler->freq_count = quanta_proportions[scheduler->level];
			}

			//Check if we're scheduling for the first time.
			if(current_thread == NULL){
				//Assign a dummy stack_pointer_t for the first context switch.
				oldsp_ptr = (stack_pointer_t) malloc(sizeof(stack_pointer_t));
			} else {
				oldsp_ptr = &(current_thread->sp);

				if(current_thread->state == FINISHED){
					queue_append(scheduler->finished_queue, current_thread);
				} else if(current_thread->state == RUNNING || current_thread->state == READY){

					//if previously idling, I shouldn't re-enqueue myself.
					if(current_thread != thread_to_run && !current_thread->idling){
						current_thread->state = READY;
						multilevel_queue_enqueue(scheduler->ready_queue, old_queue_level + 1, current_thread);
					}
				}

				current_thread->idling = 0;
			}

			thread_to_run->state = RUNNING;
			current_thread = thread_to_run;

			minithread_switch(oldsp_ptr, &(current_thread->sp));
			return 1;
		}
	}

	set_interrupt_level(old_level);
	return 0;
}


/*
* Scheduler method that makes the context switch. It adds the current TCB to the appropriate queue,
* depending on its state and then dequeues the next TCB, switching to it. It also busy waits for new threads
* to become ready, releasing them from the blocked queue if necessary.
*/
void scheduler_switch(scheduler_t scheduler){

	interrupt_level_t old_level;
	int switch_result;

	//Scheduler cannot be interrupted while it's trying to decide.
	old_level = set_interrupt_level(DISABLED);

	switch_result = scheduler_switch_dequeue(scheduler);
	if(switch_result){
		set_interrupt_level(old_level);
		return;
	}

	//If the current thread isn't finished yet and has yielded, allow it to proceed.
	if(current_thread->state == RUNNING){
			set_interrupt_level(old_level);
			return;
	}

	//There are no threads to be run and the current_thread cannot continue. Reenable interrupts and busy wait.
	current_thread->idling = 1;
	set_interrupt_level(old_level);
	while(current_thread->state != RUNNING);
	
}

/*
 * minithread_free()
 *  Frees the resources associated to t (stack and TCB). 
 */
void minithread_free(minithread_t t);


//Thread responsible for freeing up the zombie threads.
int vaccum_cleaner(int *arg){
	while(1){
		interrupt_level_t old_level;
		minithread_t zombie_thread;

		semaphore_P(cleanup_sema);

		old_level = set_interrupt_level(DISABLED);

		if(queue_dequeue(thread_scheduler->finished_queue, (void **) &zombie_thread) == 0){
			minithread_free(zombie_thread);
		}
		set_interrupt_level(old_level);
	}

}


/* Cleanup function pointer. */
int cleanup_proc(arg_t arg){
	interrupt_level_t old_level = set_interrupt_level(DISABLED);	
	current_thread->state = FINISHED;

	//Tell the vaccum_cleaner there is a thread ready to be cleaned up.
	semaphore_V(cleanup_sema);

	set_interrupt_level(old_level);

	scheduler_switch(thread_scheduler);

	//Shouldn't happen.
	return -1;
}

//Static id counter to number threads.
static int id_counter = 0;

/* minithread functions */

minithread_t minithread_fork(proc_t proc, arg_t arg) {
	interrupt_level_t old_level;

	minithread_t forked_thread; 
	forked_thread = minithread_create(proc, arg);

	old_level = set_interrupt_level(DISABLED);
	multilevel_queue_enqueue(thread_scheduler->ready_queue, 0, forked_thread);
	set_interrupt_level(old_level);


    return forked_thread;
}

minithread_t minithread_create(proc_t proc, arg_t arg) {

	minithread_t thread = (minithread_t) malloc(sizeof(minithread));
	thread->pid = id_counter++;
	thread->state = READY;

	minithread_allocate_stack(&(thread->stackbase), &(thread->stacktop));
	minithread_initialize_stack(&(thread->stacktop), 
								proc,
								arg,
								&cleanup_proc,
								NULL);

	thread->sp = thread->stacktop;

    return thread;
}

minithread_t minithread_self() {
    minithread_t self;
    interrupt_level_t old_level = set_interrupt_level(DISABLED);
    self = current_thread;
    set_interrupt_level(old_level);
    return self;
}

int minithread_id() {
    int pid;
    interrupt_level_t old_level = set_interrupt_level(DISABLED);
    pid = current_thread->pid;
    set_interrupt_level(old_level);
    return pid;
}

void minithread_stop() {
	interrupt_level_t old_level = set_interrupt_level(DISABLED);
	current_thread->state = WAITING;
	set_interrupt_level(old_level);

	scheduler_switch(thread_scheduler);
}

void minithread_start(minithread_t t) {
	interrupt_level_t old_level = set_interrupt_level(DISABLED);

	if(t->state == READY  || t->state == RUNNING) {
            set_interrupt_level(old_level);
            return;
    }
	t->state = READY;

	multilevel_queue_enqueue(thread_scheduler->ready_queue, 0, t);
	set_interrupt_level(old_level);
}

void minithread_yield() {
	scheduler_switch(thread_scheduler);
}

void minithread_free(minithread_t t){
	minithread_free_stack(t->stackbase);
	free(t);
}

/*
 * This is the clock interrupt handling routine.
 * You have to call minithread_clock_init with this
 * function as parameter in minithread_system_initialize
 */
void 
clock_handler(void* arg)
{
	interrupt_level_t old_level = set_interrupt_level(DISABLED);
	alarm_id alarm = pop_alarm();
	while(alarm != NULL){
		execute_alarm(alarm);
		deregister_alarm(alarm);
		alarm = pop_alarm();
	}
	current_tick++;
	set_interrupt_level(old_level);

	scheduler_switch_dequeue(thread_scheduler);
}

/*
 * Initialization.
 *
 *      minithread_system_initialize:
 *       This procedure should be called from your C main procedure
 *       to turn a single threaded UNIX process into a multithreaded
 *       program.
 *
 *       Initialize any private data structures.
 *       Create the idle thread.
 *       Fork the thread which should call mainproc(mainarg)
 *       Start scheduling.
 *
 */
void minithread_system_initialize(proc_t mainproc, arg_t mainarg) {

	//Allocate the scheduler's queues.
	scheduler_init(&thread_scheduler);

	//Fork the thread containing the main function.
	minithread_fork(mainproc, mainarg);

	//Fork the vaccum cleaner thread.
	minithread_fork(&vaccum_cleaner, NULL);

	//Initialize alarm system for allowing threads to sleep.
	initialize_alarm_system(MINITHREAD_CLOCK_PERIOD, &current_tick);

	//Initialize clock system for preemption.
	minithread_clock_init(MINITHREAD_CLOCK_PERIOD, clock_handler);

	//Initialize network system for remote communication.
	minisocket_initialize();
	network_initialize(minisocket_dropoff_packet);

	set_interrupt_level(ENABLED);

	//Start concurrency.
	scheduler_switch(thread_scheduler);
}

/*
 * sleep with timeout in milliseconds
 */

//A wrapper for minithread_start. 
void wrapper_minithread_start(void *arg){
	minithread_t t = (minithread_t) arg;
	minithread_start(t);
}

void semaphore_V_wrapper(void *semaphore_ptr) {
        semaphore_t semaphore = (semaphore_t) semaphore_ptr;
        semaphore_V(semaphore);
}

void 
minithread_sleep_with_timeout(int delay)
{
	semaphore_t sleep_sema = semaphore_create();

    semaphore_initialize(sleep_sema, 0);

	register_alarm(delay, semaphore_V_wrapper, sleep_sema);
	semaphore_P(sleep_sema);

	semaphore_destroy(sleep_sema);
}




