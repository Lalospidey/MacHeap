#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <fcntl.h>
#include <pthread.h>
#include <x86intrin.h>

// shared between all threads
struct {
    unsigned long count;
    unsigned int tsc;
}*gp_samples;

volatile int gi_samples;
volatile int gi_running;
volatile int g_fd;

// begin code
__attribute__((always_inline))
int
get_core_number()
{
    char *p;
    struct {
        uint16_t sel;
        void* address;
    } idtr;
    asm("sidt %0\n\t": "=m"(idtr));

    return idtr.sel & 0x1f;
}

__attribute__((always_inline))
long
core_spin()
{
    register int core;
    register unsigned long count;

    // start counting
    count = 0;
    core = get_core_number();
    while (get_core_number() == core)
        count++;
    return count;
}

__attribute__((always_inline))
void
core_wait()
{
    register int core;
    // wait for core switch
    core = get_core_number();
    while (get_core_number() == core);
}

void
sample(int iterations)
{
    register long long tsc;

    // take some number of samples
    core_wait();
    while (iterations-- > 0) {
        tsc = __rdtsc();
        gp_samples[gi_samples].count = core_spin();
        gp_samples[gi_samples].tsc = (unsigned int)(__rdtsc() - tsc);
        gi_samples++;
    }
}

void*
threadentry_sample(void* args)
{
    int iterations = (int)(args);
    core_wait();
    sample(iterations);
    return NULL;
}

void*
threadentry_busy(void* args)
{
    char buffer[0x400];
    while (gi_running) {
        read(g_fd, &buffer[0], sizeof(buffer));
    }
    return NULL;
}

int
main(int argc, char** argv)
{
    int iterations;
    pthread_t busy_t, sample_t;
    double agg_iterations, agg_ticks;

    if (argc != 2)
        return 1;

    // figure out how many samples to take
    iterations = atoi(argv[1]);
    gp_samples = calloc(sizeof(*gp_samples), iterations);
    if (gp_samples == NULL) {
        printf("Unable to allocate space for sample data\n");
        return 1;
    }
    gi_samples = 0;

    // open up an fd so we can read from it
    g_fd = open("/dev/zero", O_RDONLY);
    if (g_fd == -1) {
        printf("Unable to create a dummy file-descriptor\n");
        return 1;
    }

    // spawn busy thread
    if (pthread_create(&busy_t, NULL, threadentry_busy, NULL)) {
        printf("Unable to create busy thread\n");
        return 1;
    }
    printf("Created busy thread\n");

    // spawn sampling thread
    if (pthread_create(&sample_t, NULL, threadentry_sample, (void*)(iterations))) {
        printf("Unable to create sampling thread\n");
        return 1;
    }
    printf("Created sampling thread\n");

    // wait until sampler is done
    if (pthread_join(sample_t, NULL)) {
        printf("Joining on sampling thread failed\n");
    }
    gi_running = 0;
    if (pthread_join(busy_t, NULL)) {
        printf("Joining on busy thread failed\n");
    }

    // some cleanup
    close(g_fd);

    // output results
    printf("Results for %d samples:\n", gi_samples);
    for (iterations = 0; iterations < gi_samples; iterations++) {
        printf("[%d] iterations=%ld ticks=%d\n", iterations, gp_samples[iterations].count, gp_samples[iterations].tsc);
        agg_iterations = (double)gp_samples[iterations].count;
        agg_ticks = (double)gp_samples[iterations].tsc;
    }
    free(gp_samples);

    printf("avg iterations: %lf\n", agg_iterations / (double)gi_samples);
    printf("avg microseconds: %lf\n", agg_ticks / (double)gi_samples);
    return 0;
}
