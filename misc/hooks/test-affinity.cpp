#include <stdint.h>
#include <stdio.h>
#include "unistd.h"
#include "pthread.h"
#include "sched.h"

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
    int tid;
    uint64_t ret;
};

static void* thread_function(void* th_args) {
    thread_arg_t* args = (thread_arg_t*)th_args;
    int tid = args->tid;

    printf("Thread %d: start on core %u\n", tid, get_cpuid());

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


    // Pthread affinity API.
    CPU_ZERO(&set);
    CPU_SET(tid + 8, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &set);

    CPU_ZERO(&set);
    pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &set);
    for (int i = 0; i < (int)sizeof(cpu_set_t)*8; i++) {
        if (CPU_ISSET(i, &set)) printf("Thread %d: could run on core %d\n", tid, i);
    }
    printf("Thread %d: actual running on core %u\n", tid, get_cpuid());

    args->ret = dummy_compute(tid);

    return NULL;
}

int main() {
    printf("zsim sched_get/setaffinity test\n");
    printf("sizeof(cpu_set_t) == %lu\n", sizeof(cpu_set_t));

    pthread_t threads[4];
    thread_arg_t thread_args[4];
    for (uint32_t tid = 0; tid < 4; tid++) {
        thread_args[tid].tid = tid;
        pthread_create(&threads[tid], NULL, thread_function, &thread_args[tid]);
    }
    for (uint32_t tid = 0; tid < 4; tid++) {
        pthread_join(threads[tid], NULL);
    }

    printf("zsim sched_get/setaffinity test done\n");

    return 0;
}


