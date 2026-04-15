/*
 * io_stress.c â€” I/O-bound workload for scheduler experiments
 *
 * Usage: ./io_stress <seconds>
 *
 * Repeatedly writes a 1 MB buffer to a temp file then reads it back,
 * forcing the process to spend most of its time blocked on I/O. The
 * scheduler experiment compares this against a CPU-bound container.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BUF_SIZE (1024 * 1024) /* 1 MB */
#define TMP_FILE "/tmp/io_stress_tmp"

int main(int argc, char *argv[]) {
    int duration = 10;
    if (argc >= 2)
        duration = atoi(argv[1]);

    printf("[io_stress] PID=%d  running for %d seconds\n", (int)getpid(), duration);
    fflush(stdout);

    char *buf = malloc(BUF_SIZE);
    if (!buf) { perror("malloc"); return 1; }
    memset(buf, 0xAB, BUF_SIZE);

    long iterations = 0;
    time_t start = time(NULL);

    while ((time(NULL) - start) < duration) {
        /* Write */
        FILE *f = fopen(TMP_FILE, "wb");
        if (!f) { perror("fopen write"); break; }
        fwrite(buf, 1, BUF_SIZE, f);
        fclose(f);

        /* Read back */
        f = fopen(TMP_FILE, "rb");
        if (!f) { perror("fopen read"); break; }
        fread(buf, 1, BUF_SIZE, f);
        fclose(f);

        iterations++;
    }

    free(buf);
    unlink(TMP_FILE);
    printf("[io_stress] PID=%d  done (%ld iterations)\n", (int)getpid(), iterations);
    return 0;
}

