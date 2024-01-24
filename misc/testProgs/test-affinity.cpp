#include <stdint.h>
#include <stdio.h>
#include "unistd.h"
#include "pthread.h"
#include "sched.h"
#include "sys/syscall.h"
#include "sys/types.h"

static inline uint32_t get_cpuid() {
    return sched_getcpu();
}

// Thread excutes different numbers of instructions based on its thread id.
// Check zsim.out for instruction counts on the pinning cores.
static uint64_t dummy_compute(uint64_t amount) {
    uint64_t ret = 0;
    const uint64_t amplify = 1uL << 23;
    for (uint64_t i = 0; i < amount * amplify; i++) ret += i;
    return ret;
}

struct thread_arg_t {
    pid_t* pids;
    pthread_barrier_t* bar;
    int tid;
    uint64_t ret;
};

static void* thread_function(void* th_args) {
    thread_arg_t* args = (thread_arg_t*)th_args;
    int tid = args->tid;
    pid_t* pids = args->pids;
    pthread_barrier_t* bar = args->bar;

    printf("Thread %d: start on core %u\n", tid, get_cpuid());

    pids[tid] = syscall(SYS_gettid);

    pthread_barrier_wait(bar);

    // syscall affinity API.
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(tid + 4, &set);
    sched_setaffinity(0, sizeof(cpu_set_t), &set);

    CPU_ZERO(&set);
    sched_getaffinity(0, sizeof(cpu_set_t), &set);
    for (int i = 0; i < (int)sizeof(cpu_set_t)*8; i++) {
        if (CPU_ISSET(i, &set)) printf("Thread %d: could run on core %d\n", tid, i);
    }
    printf("Thread %d: actual running on core %u\n", tid, get_cpuid());

    args->ret = dummy_compute(tid);

    if (pthread_barrier_wait(bar) == PTHREAD_BARRIER_SERIAL_THREAD) {
        printf("Round 1 done.\n");
    }


    // Pthread affinity API.
    // Use dynamically sized cpu set.
    cpu_set_t* pset = CPU_ALLOC(2048);
    size_t size = CPU_ALLOC_SIZE(2048);
    CPU_ZERO_S(size, pset);
    CPU_SET_S(tid + 8, size, pset);
    pthread_setaffinity_np(pthread_self(), size, pset);

    CPU_ZERO_S(size, pset);
    pthread_getaffinity_np(pthread_self(), size, pset);
    for (int i = 0; i < (int)size*8; i++) {
        if (CPU_ISSET_S(i, size, pset)) printf("Thread %d: could run on core %d\n", tid, i);
    }
    printf("Thread %d: actual running on core %u\n", tid, get_cpuid());
    CPU_FREE(pset);

    args->ret = dummy_compute(tid);

    if (pthread_barrier_wait(bar) == PTHREAD_BARRIER_SERIAL_THREAD) {
        printf("Round 2 done.\n");
    }


    // Set affinity for others.
    CPU_ZERO(&set);
    CPU_SET(tid + 12, &set);
    sched_setaffinity(pids[(tid+2)%4], sizeof(cpu_set_t), &set);

    // Wait on barrier to ensure affinity has been set.
    pthread_barrier_wait(bar);

    CPU_ZERO(&set);
    sched_getaffinity(pids[(tid+2)%4], sizeof(cpu_set_t), &set);
    for (int i = 0; i < (int)sizeof(cpu_set_t)*8; i++) {
        if (CPU_ISSET(i, &set)) printf("Thread %d: could run on core %d\n", (tid+2)%4, i);
    }
    printf("Thread %d: actual running on core %u\n", tid, get_cpuid());

    args->ret = dummy_compute(tid);

    if (pthread_barrier_wait(bar) == PTHREAD_BARRIER_SERIAL_THREAD) {
        printf("Round 3 done.\n");
    }

    return NULL;
}

int main() {
    printf("zsim sched_get/setaffinity test\n");
    printf("sizeof(cpu_set_t) == %lu\n", sizeof(cpu_set_t));

    pthread_t threads[4];
    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, 4);
    pid_t pids[4];
    thread_arg_t thread_args[4];
    for (uint32_t tid = 0; tid < 4; tid++) {
        thread_args[tid].pids = pids;
        thread_args[tid].bar = &barrier;
        thread_args[tid].tid = tid;
        pthread_create(&threads[tid], NULL, thread_function, &thread_args[tid]);
    }
    for (uint32_t tid = 0; tid < 4; tid++) {
        pthread_join(threads[tid], NULL);
    }
    pthread_barrier_destroy(&barrier);

    printf("zsim sched_get/setaffinity test done\n");

    return 0;
}


