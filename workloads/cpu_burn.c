/*
 * cpu_burn.c â€” CPU-bound workload for scheduler experiments
 *
 * Usage: ./cpu_burn <seconds>
 *
 * Spins doing floating-point arithmetic so the process stays fully
 * CPU-bound for the requested duration. The scheduler experiment
 * compares two instances of this running at different nice values.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

int main(int argc, char *argv[]) {
    int duration = 10; /* default: 10 seconds */
    if (argc >= 2)
        duration = atoi(argv[1]);

    printf("[cpu_burn] PID=%d  running for %d seconds\n", (int)getpid(), duration);
    fflush(stdout);

    time_t start = time(NULL);
    volatile double x = 1.0000001;

    while ((time(NULL) - start) < duration) {
        /* Tight floating-point loop â€” keeps one CPU core at ~100% */
        x = x * 1.0000001 + sqrt(x);
        if (x > 1e300) x = 1.0; /* prevent overflow */
    }

    printf("[cpu_burn] PID=%d  done (result=%f)\n", (int)getpid(), x);
    return 0;
}

