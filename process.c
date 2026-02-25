/*
 * process.c - Command spawning
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "process.h"

/* Join argv into a single space-separated string for sh -c */
static char *join_args(char **argv) {
  size_t len = 0;
  for (int i = 0; argv[i]; i++)
    len += strlen(argv[i]) + 1;
  char *buf = malloc(len);
  if (!buf) {
    perror("sash: malloc");
    exit(1);
  }
  char *p = buf;
  for (int i = 0; argv[i]; i++) {
    if (i > 0)
      *p++ = ' ';
    size_t n = strlen(argv[i]);
    memcpy(p, argv[i], n);
    p += n;
  }
  *p = '\0';
  return buf;
}

pid_t spawn_command(char **cmd_argv, bool use_exec, int *read_fd) {
  int pipefd[2];
  if (pipe(pipefd) == -1) {
    perror("sash: pipe");
    exit(1);
  }

  pid_t pid = fork();
  if (pid == -1) {
    perror("sash: fork");
    exit(1);
  }

  if (pid == 0) {
    /* child */
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    if (use_exec) {
      execvp(cmd_argv[0], cmd_argv);
    } else {
      char *cmd = join_args(cmd_argv);
      execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
      free(cmd);
    }
    perror("sash: exec");
    _exit(127);
  }

  /* parent */
  close(pipefd[1]);
  *read_fd = pipefd[0];
  return pid;
}
