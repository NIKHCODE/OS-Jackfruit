#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    int mb = 0;
    printf("[mem_hog] PID=%d starting\n", getpid());
    fflush(stdout);
    while (1) {
        char *p = malloc(1024 * 1024); /* 1 MB */
        if (!p) break;
        memset(p, 0xAB, 1024 * 1024); /* actually touch pages = real RSS */
        mb++;
        printf("[mem_hog] allocated %d MB\n", mb);
        fflush(stdout);
        sleep(1);
    }
    return 0;
}
