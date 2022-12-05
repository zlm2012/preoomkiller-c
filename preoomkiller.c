#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

#define BUF_LEN 1024

#define SIGADDSET(sigset, signal)                                              \
    {                                                                          \
        if(sigaddset(&sigset, signal) != 0) {                                  \
            perror("sigaddset");                                               \
            exit(EXIT_FAILURE);                                                \
        }                                                                      \
    }

#define EXEC_FALLBACK(emsg)                                                    \
    {                                                                          \
        perror(emsg);                                                          \
        goto cexec;                                                            \
    }

void exec_child(char *argv[]) {
    execvp(argv[0], argv);
    if(errno != 0)
        perror(strerror(errno));
    exit(1);
}

static struct option long_options[] = {
    {"hook", optional_argument, NULL, 'h'},
    {"percent", optional_argument, NULL, 'p'},
    {NULL, 0, NULL, 0}};

void usage(char *cmd) {
    fprintf(
        stderr, "%s [-h <one-line-shell>] [-p <pre-oom percentage>] -- <cmd to run>\n", cmd);
    fprintf(
        stderr, "args: -h | --hook:    one-line-shell to be run on pre-oom. optional\n");
    fprintf(
        stderr, "      -p | --percent: pre-oom threshold percentage. default: 90%%\n");
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
    int c, option_index;
    const char *optstring = "h:p:";
    opterr = 0;
    char *hook = NULL;
    double mem_percentage = 90;

    while(1) {
        option_index = 0;

        if((c = getopt_long(argc, argv, optstring, long_options,
                            &option_index)) == -1) {
            break;
        }
        switch(c) {
        case 0:
            if(option_index == 0) {
                hook = optarg;
            }
            if(option_index == 1) {
                if(sscanf(optarg, "%lf", &mem_percentage) != 1)
                    usage(argv[0]);
                if(mem_percentage >= 100.0 && mem_percentage <= 0.0)
                    usage(argv[0]);
            }
            break;
        case 'h':
            hook = optarg;
            break;
        case 'p':
            if(sscanf(optarg, "%lf", &mem_percentage) != 1)
                usage(argv[0]);
            if(mem_percentage >= 100.0 && mem_percentage <= 0.0)
                usage(argv[0]);
            break;
        default:
            usage(argv[0]);
        }
    }

    sigset_t ss, oldss;
    int ret, signo;
    int64_t mem_max = -1, mem_cur;
    FILE *fp;
    char buf[BUF_LEN];

    // check cgroup existance & version
    if(access("/sys/fs/cgroup", F_OK) != 0) {
        EXEC_FALLBACK("cgroup does not exist, just exec...");
    }
    // check if cgroup version
    if(access("/sys/fs/cgroup/memory", X_OK) == 0) {
        // cgroup v1
        // read max memory
        fp = fopen("/sys/fs/cgroup/memory/memory.stat", "r");
        if(fp == NULL) {
            EXEC_FALLBACK("failed on read memory.stat, just exec...");
        }
        while(fgets(buf, BUF_LEN, fp) != NULL) {
            const char *prefix = "hierarchical_memory_limit";
            if(strncmp(prefix, buf, strlen(prefix)) == 0) {
                sscanf(buf, "hierarchical_memory_limit %ld", &mem_max);
                if(mem_max == -1 || mem_max == INT64_MAX) {
                    EXEC_FALLBACK("no max memory limit, just exec...");
                }
                fclose(fp);
                // open current memory usage file
                fp = fopen("/sys/fs/cgroup/memory/memory.usage_in_bytes", "r");
                if(fp == NULL) {
                    EXEC_FALLBACK(
                        "failed on read memory.usage_in_bytes, just exec...");
                }
                break;
            }
        }
        if(feof(fp)) {
            EXEC_FALLBACK("no max memory limit, just exec...");
        }
    } else if(access("/sys/fs/cgroup/memory.max", F_OK) == 0) {
        // cgroup v2
        // read max memory
        fp = fopen("/sys/fs/cgroup/memory.max", "r");
        if(fp == NULL) {
            EXEC_FALLBACK("failed on read memory.max, just exec...");
        }
        if(fgets(buf, BUF_LEN, fp) != NULL) {
            if(strncmp(buf, "max", 3) == 0) {
                EXEC_FALLBACK("no max memory limit, just exec...");
            }
            errno = 0;
            mem_max = strtoll(buf, NULL, 10);
            if(errno != 0) {
                EXEC_FALLBACK("failed on reading memory limit, just exec...");
            }
            fp = fopen("/sys/fs/cgroup/memory.current", "r");
            if(fp == NULL) {
                EXEC_FALLBACK("failed on read memory.current, just exec...");
            }
        }
    } else {
        EXEC_FALLBACK("no cgroup memory conf detected, just exec...");
    }

    // set soft mem limit
    mem_max = (int64_t) round(mem_max / 100.0 * mem_percentage);

    // prepare for signal handling (via sigwait)
    sigemptyset(&ss);
    SIGADDSET(ss, SIGHUP);
    SIGADDSET(ss, SIGINT);
    SIGADDSET(ss, SIGQUIT);
    SIGADDSET(ss, SIGTERM);
    SIGADDSET(ss, SIGALRM);
    SIGADDSET(ss, SIGCHLD);
    ret = sigprocmask(SIG_BLOCK, &ss, &oldss);
    if(ret != 0)
        exit(EXIT_FAILURE);

    // fork
    pid_t cpid = fork();
    if(cpid == -1) {
        perror("fork");
        exit(EXIT_FAILURE);
    }

    // exec (child)
    if(cpid == 0) {
        ret = sigprocmask(SIG_SETMASK, &oldss, NULL);
        if(ret != 0) {
            perror("sigprocmask(restore)");
            exit(EXIT_FAILURE);
        }
    cexec:
        exec_child(argv + optind);
    }

    // parent handling starts here
    // set timer
    struct itimerval timer;
    timer.it_value.tv_sec = 1;
    timer.it_value.tv_usec = 0;
    timer.it_interval.tv_sec = 1;
    timer.it_interval.tv_usec = 0;
    if(setitimer(ITIMER_REAL, &timer, NULL) < 0) {
        perror("setitimer error");
        exit(EXIT_FAILURE);
    }

    // signal handling
    int wstatus;
    int wpid, hpid = -1;
    int child_exited = 0, preoom = 0, hook_exited = 0;

    while(!child_exited) {
        if(sigwait(&ss, &signo) == 0) {
            switch(signo) {
            case SIGCHLD:
                // child process status changed, check if exited
                // (ignore possible pre-oom hook)
                wpid = waitpid(-1, &wstatus, WNOHANG);
                if(wpid == cpid) {
                    child_exited = 1;
                }
                if(hpid > 0 && hpid == wpid) {
                    hook_exited = 1;
                }
                break;
            case SIGINT:
            case SIGQUIT:
            case SIGTERM:
            case SIGHUP:
                // forward signals to child
                fprintf(stderr, "forward kill %d %d\n", cpid, signo);
                ret = kill(cpid, signo);
                if(ret == -1) {
                    perror("kill (forward)");
                }
                break;
            case SIGALRM:
                if(preoom) {
                    // preoom already handled
                    break;
                }
                // check memory usage periodically
                rewind(fp);
                if(fgets(buf, BUF_LEN, fp) != NULL) {
                    errno = 0;
                    mem_cur = strtoll(buf, NULL, 10);
                    if(errno != 0) {
                        perror("failed on reading current memory usage. exit.");
                        exit(EXIT_FAILURE);
                    }
                    //fprintf(stderr, "cur/max %ld/%ld\n", mem_cur, mem_max);
                    if(mem_cur > mem_max) {
                        // pre oom
                        preoom = 1;
                        ret = kill(cpid, SIGTERM);
                        if(ret == -1) {
                            perror("kill (pre-oom)");
                            child_exited = 1; // force quit
                        }

                        // pre oom hook if set
                        if(hook != NULL) {
                            // fork
                            hpid = fork();
                            if(hpid == -1) {
                                perror("fork");
                                exit(EXIT_FAILURE);
                            }

                            // exec (child)
                            if(hpid == 0) {
                                char *hook_cmd[] = {"/bin/sh", "-c", hook,
                                                    NULL};
                                ret = sigprocmask(SIG_SETMASK, &oldss, NULL);
                                if(ret != 0) {
                                    perror("sigprocmask(restore)");
                                    exit(EXIT_FAILURE);
                                }
                                exec_child(hook_cmd);
                            }
                        }
                    }
                }
                break;
            }
        }
    }

    if(hook != NULL && !hook_exited) {
        // parent
        wpid = waitpid(hpid, NULL, 0);
    }
    return wstatus;
}
