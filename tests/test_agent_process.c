#define main eventnet_agent_program_main
#include "../examples/eventnet_agent.c"
#undef main

#include <dirent.h>

static int descriptor_count(void)
{
    DIR *directory = opendir("/proc/self/fd");
    if (directory == NULL) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) count++;
    }
    closedir(directory);
    return count;
}

int main(int argument_count, char **arguments)
{
    if (argument_count != 4) return 2;
    int closed_mask = atoi(arguments[1]);
    bool expected_success = strcmp(arguments[2], "healthy") == 0;
    if (setenv("PATH", arguments[3], 1) != 0) return 3;
    for (int descriptor = 0; descriptor <= STDERR_FILENO; descriptor++) {
        if (closed_mask & (1 << descriptor)) close(descriptor);
    }
    int baseline = descriptor_count();
    if (baseline < 0) return 4;
    for (int repetition = 0; repetition < 30; repetition++) {
        double rtt_ms = -1.0;
        double loss_percent = -1.0;
        bool success = measure_ping("127.0.0.1", &rtt_ms, &loss_percent);
        if (success != expected_success || loss_percent != (success ? 0.0 : 100.0)) return 5;
        if (rtt_ms != (success ? 1.25 : 0.0)) return 6;
        if (descriptor_count() != baseline) return 7;
        errno = 0;
        if (waitpid(-1, NULL, WNOHANG) != -1 || errno != ECHILD) return 8;
    }
    return 0;
}
