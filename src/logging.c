#include "logging.h"

#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

void print_welcome(void) {
        FILE *file = fopen("/etc/os-release", "r");
        if (file == NULL) {
                file = fopen("/usr/lib/os-release", "r");
        } else if (file == NULL) {
                goto generic;
        }

        char line[128];
        int found = 0;

        while (fgets(line, sizeof(line), file) != NULL) {
                if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
                        char *name_start = line + 12;

                        if (*name_start == '"') name_start++;
                        size_t len = strlen(name_start);

                        if (len > 0 && name_start[len - 1] == '\n') name_start[--len] = '\0';
                        if (len > 0 && name_start[len - 1] == '"') name_start[--len] = '\0';

                        printf("Welcome to %s!\n", name_start);
                        found = 1;
                        break;
                }
        }
generic:
        if (!found) {
                printf("Welcome to Linux!\n"); // maytbe we fallback to uname? 
        }
}