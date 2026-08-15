#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

/**
 * Выполняет одну команду в дочернем процессе.
 */
static void
exec_command(struct command *cmd, int stdin_fd, int stdout_fd)
{
	if (stdin_fd != STDIN_FILENO) {
		dup2(stdin_fd, STDIN_FILENO);
		close(stdin_fd);
	}
	if (stdout_fd != STDOUT_FILENO) {
		dup2(stdout_fd, STDOUT_FILENO);
		close(stdout_fd);
	}

	char **argv = malloc(sizeof(char *) * (cmd->arg_count + 2));
	argv[0] = cmd->exe;
	for (uint32_t i = 0; i < cmd->arg_count; ++i) {
		argv[i + 1] = cmd->args[i];
	}
	argv[cmd->arg_count + 1] = NULL;

	execvp(cmd->exe, argv);

	fprintf(stderr, "%s: command not found\n", cmd->exe);
	free(argv);
	_exit(127);
}

/**
 * Выполняет один конвейер (pipeline) команд.
 */
static int
execute_pipeline(struct expr *start, uint32_t cmd_count, 
				 const struct command_line *line, int is_last_pipeline, int prev_status)
{
	int is_builtin_context = (cmd_count == 1 && line->out_type == OUTPUT_TYPE_STDOUT);

	if (is_builtin_context && strcmp(start->cmd.exe, "cd") == 0) {
		if (start->cmd.arg_count > 0) {
			if (chdir(start->cmd.args[0]) != 0) {
				fprintf(stderr, "cd: %s: %s\n", start->cmd.args[0], strerror(errno));
				return 1;
			}
		} else {
			char *home = getenv("HOME");
			if (home) {
				if (chdir(home) != 0) {
					fprintf(stderr, "cd: %s: %s\n", home, strerror(errno));
					return 1;
				}
			} else {
				fprintf(stderr, "cd: HOME not set\n");
				return 1;
			}
		}
		return 0;
	}

	// Обработка exit: если нет аргументов, используем статус предыдущей команды
	if (is_builtin_context && strcmp(start->cmd.exe, "exit") == 0) {
		int code = prev_status;
		if (start->cmd.arg_count > 0) {
			code = atoi(start->cmd.args[0]);
		}
		exit(code);
	}

	int prev_pipe[2] = {-1, -1};
	int next_pipe[2] = {-1, -1};
	pid_t *pids_arr = malloc(sizeof(pid_t) * cmd_count);

	for (uint32_t i = 0; i < cmd_count; ++i) {
		struct expr *e = start;
		for (uint32_t j = 0; j < i; ++j) {
			e = e->next->next;
		}

		if (i < cmd_count - 1) {
			if (pipe(next_pipe) == -1) {
				perror("pipe");
				free(pids_arr);
				return 1;
			}
		}

		pid_t pid = fork();
		if (pid == -1) {
			perror("fork");
			if (i > 0) {
				close(prev_pipe[0]);
				close(prev_pipe[1]);
			}
			if (i < cmd_count - 1) {
				close(next_pipe[0]);
				close(next_pipe[1]);
			}
			free(pids_arr);
			return 1;
		}

		if (pid == 0) {
			// --- Дочерний процесс ---
			if (i > 0) {
				dup2(prev_pipe[0], STDIN_FILENO);
				close(prev_pipe[0]);
				close(prev_pipe[1]);
			}
			if (i < cmd_count - 1) {
				dup2(next_pipe[1], STDOUT_FILENO);
				close(next_pipe[0]);
				close(next_pipe[1]);
			}

			int stdout_fd = STDOUT_FILENO;
			if (i == cmd_count - 1 && is_last_pipeline && 
				line->out_type != OUTPUT_TYPE_STDOUT) {
				int flags = O_WRONLY | O_CREAT;
				if (line->out_type == OUTPUT_TYPE_FILE_NEW) flags |= O_TRUNC;
				else flags |= O_APPEND;
				
				stdout_fd = open(line->out_file, flags, 0644);
				if (stdout_fd == -1) {
					perror("open");
					_exit(1);
				}
			}

			// В подшелле exit тоже использует prev_status
			if (strcmp(e->cmd.exe, "exit") == 0) {
				int code = prev_status;
				if (e->cmd.arg_count > 0) code = atoi(e->cmd.args[0]);
				_exit(code);
			}
			if (strcmp(e->cmd.exe, "cd") == 0) {
				_exit(0);
			}

			exec_command(&e->cmd, STDIN_FILENO, stdout_fd);
		} else {
			// --- Родительский процесс ---
			pids_arr[i] = pid;
			if (i > 0) {
				close(prev_pipe[0]);
				close(prev_pipe[1]);
			}
			if (i < cmd_count - 1) {
				prev_pipe[0] = next_pipe[0];
				prev_pipe[1] = next_pipe[1];
			}
		}
	}

	int status = 0;
	int is_bg = line->is_background && is_last_pipeline;

	if (!is_bg) {
		for (uint32_t i = 0; i < cmd_count; ++i) {
			int wstatus;
			waitpid(pids_arr[i], &wstatus, 0);
			if (i == cmd_count - 1) {
				if (WIFEXITED(wstatus)) status = WEXITSTATUS(wstatus);
				else if (WIFSIGNALED(wstatus)) status = 128 + WTERMSIG(wstatus);
			}
		}
	}

	free(pids_arr);
	return status;
}

/**
 * Выполняет всю командную строку.
 */
static int
execute_command_line(const struct command_line *line, int prev_status)
{
	struct expr *current = line->head;
	int last_status = prev_status;

	while (current != NULL) {
		struct expr *pipeline_start = current;
		uint32_t cmd_count = 0;

		while (current != NULL && current->type == EXPR_TYPE_COMMAND) {
			cmd_count++;
			if (current->next != NULL && current->next->type == EXPR_TYPE_PIPE) {
				current = current->next->next;
			} else {
				current = current->next;
				break;
			}
		}

		int is_last_pipeline = (current == NULL);

		if (cmd_count > 0) {
			last_status = execute_pipeline(pipeline_start, cmd_count, line, is_last_pipeline, last_status);
		}

		// Пропускаем операторы && и || (так как бонус отключен)
		if (current != NULL && (current->type == EXPR_TYPE_AND || current->type == EXPR_TYPE_OR)) {
			current = current->next;
		}
	}
	return last_status;
}

int
main(void)
{
	const size_t buf_size = 1024;
	char buf[buf_size];
	int rc;
	struct parser *p = parser_new();
	int last_status = 0; // Храним статус последней команды
	
	while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
		// Собираем зомби от фоновых задач
		while (waitpid(-1, NULL, WNOHANG) > 0) {}

		parser_feed(p, buf, rc);
		struct command_line *line = NULL;
		
		while (1) {
			enum parser_error err = parser_pop_next(p, &line);
			if (err == PARSER_ERR_NONE && line == NULL)
				break;
			if (err != PARSER_ERR_NONE) {
				continue;
			}
			last_status = execute_command_line(line, last_status);
			command_line_delete(line);
		}
	}
	
	parser_delete(p);
	// Возвращаем ОС код возврата последней выполненной команды
	return last_status;
}