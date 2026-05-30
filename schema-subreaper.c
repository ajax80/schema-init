#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <errno.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <program> [args...]\n", argv[0]);
        return 1;
    }

    /* Set self as a subreaper to adopt orphaned grandchildren */
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) < 0) {
        perror("schema-subreaper: prctl(PR_SET_CHILD_SUBREAPER) failed");
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("schema-subreaper: fork failed");
        return 1;
    }

    if (pid == 0) {
        /* Child: execute target daemon */
        execvp(argv[1], &argv[1]);
        perror("schema-subreaper: execvp failed");
        _exit(127);
    }

    /* Parent: wait for all descendant processes to exit */
    int status = 0;
    int last_status = 0;
    pid_t reaped;

    while ((reaped = waitpid(-1, &status, 0)) > 0) {
        if (WIFEXITED(status)) {
            last_status = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            last_status = 128 + WTERMSIG(status);
        }
    }

    if (reaped < 0 && errno != ECHILD) {
        perror("schema-subreaper: waitpid error");
        return 1;
    }

    /* Exit with the exit status of the last reaped child process */
    return last_status;
}
