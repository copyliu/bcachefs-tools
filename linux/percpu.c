/*
 * Userspace shim for kernel-style DEFINE_PER_CPU.
 *
 * DEFINE_PER_CPU(type, name) places `name` in the linker section
 * "bch_percpu". The linker auto-generates __start_bch_percpu /
 * __stop_bch_percpu symbols delimiting that section.
 *
 * Each thread that participates gets a private chunk of size
 * (__stop_bch_percpu - __start_bch_percpu) bytes, allocated and pointed
 * at by the TLS variable bch_percpu_my_chunk. The variable's address
 * within the section is its offset within the chunk.
 *
 * Cross-thread access goes through bch_percpu_chunks[cpu], a global
 * array of per-thread chunk pointers, indexed by a thread slot id
 * assigned at first init.
 *
 * Threads must call bch_percpu_thread_init() once before any percpu
 * access (kthread_start_fn() and the fuse worker init paths do this).
 * Subsystems with state that needs setup/teardown per thread register
 * via bch_percpu_register(); their init_one is called for the new
 * thread's instance at thread create, exit_one at module shutdown for
 * every live chunk.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/percpu.h>

extern char __start_bch_percpu[], __stop_bch_percpu[];

__thread void *bch_percpu_my_chunk;
__thread int   bch_percpu_my_id = -1;

void *bch_percpu_chunks[BCH_PERCPU_MAX_CPUS];
int   bch_percpu_nr_cpus;

#define BCH_PERCPU_MAX_CALLBACKS 32

struct bch_percpu_callback {
	void (*init_one)(void *);
	void (*exit_one)(void *);
	void *pcv;
};

static struct bch_percpu_callback callbacks[BCH_PERCPU_MAX_CALLBACKS];
static int		nr_callbacks;
static pthread_mutex_t	callbacks_lock = PTHREAD_MUTEX_INITIALIZER;

void bch_percpu_register(void (*init_one)(void *),
			 void (*exit_one)(void *),
			 void *pcv)
{
	pthread_mutex_lock(&callbacks_lock);

	if (nr_callbacks == BCH_PERCPU_MAX_CALLBACKS) {
		pthread_mutex_unlock(&callbacks_lock);
		fprintf(stderr, "bch_percpu_register: callback table full\n");
		abort();
	}

	int idx = nr_callbacks++;
	callbacks[idx] = (struct bch_percpu_callback){ init_one, exit_one, pcv };

	/*
	 * Threads created before this call missed the init for this callback;
	 * run it now for every existing chunk so they can use the variable.
	 */
	for (int cpu = 0; cpu < bch_percpu_nr_cpus; cpu++)
		if (bch_percpu_chunks[cpu] && init_one)
			init_one(__bch_percpu_resolve(pcv, bch_percpu_chunks[cpu]));

	pthread_mutex_unlock(&callbacks_lock);
}

void bch_percpu_thread_init(void)
{
	if (bch_percpu_my_chunk)
		return;

	size_t size = __stop_bch_percpu - __start_bch_percpu;
	void *chunk = calloc(1, size);
	if (!chunk) {
		fprintf(stderr, "bch_percpu_thread_init: out of memory\n");
		abort();
	}

	pthread_mutex_lock(&callbacks_lock);

	int my_id = bch_percpu_nr_cpus++;
	if (my_id >= BCH_PERCPU_MAX_CPUS) {
		pthread_mutex_unlock(&callbacks_lock);
		fprintf(stderr, "bch_percpu_thread_init: too many threads (max %d)\n",
			BCH_PERCPU_MAX_CPUS);
		abort();
	}

	bch_percpu_my_chunk = chunk;
	bch_percpu_my_id    = my_id;
	bch_percpu_chunks[my_id] = chunk;

	for (int i = 0; i < nr_callbacks; i++)
		if (callbacks[i].init_one)
			callbacks[i].init_one(__bch_percpu_resolve(callbacks[i].pcv, chunk));

	pthread_mutex_unlock(&callbacks_lock);
}

/*
 * Run before any module_init() (priority 120): module_init constructors
 * are kernel-mirror code that may iterate for_each_possible_cpu() over
 * DEFINE_PER_CPU storage; that needs slot 0 to exist with a real chunk
 * before they run. Allocates slot 0 in the calling thread's TLS, which
 * is the main thread (constructors run on it).
 */
__attribute__((constructor(110)))
static void bch_percpu_module_init(void)
{
	bch_percpu_thread_init();
}

__attribute__((destructor))
static void bch_percpu_module_exit(void)
{
	pthread_mutex_lock(&callbacks_lock);
	for (int cpu = 0; cpu < bch_percpu_nr_cpus; cpu++) {
		void *chunk = bch_percpu_chunks[cpu];
		if (!chunk)
			continue;

		for (int i = nr_callbacks - 1; i >= 0; i--)
			if (callbacks[i].exit_one)
				callbacks[i].exit_one(__bch_percpu_resolve(callbacks[i].pcv, chunk));

		free(chunk);
		bch_percpu_chunks[cpu] = NULL;
	}
	pthread_mutex_unlock(&callbacks_lock);
}
