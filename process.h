/*
 * process.h - Command spawning
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef PROCESS_H
#define PROCESS_H

#include <stdbool.h>
#include <sys/types.h>

pid_t spawn_command(char **cmd_argv, bool use_exec, int *read_fd);

#endif /* PROCESS_H */
