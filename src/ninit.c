#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

int main(void) {
        if (getpid() != 1) {
                fprintf(stderr, "Error: must run as PID 1.\n");
                return EXIT_FAILURE;
        }
}