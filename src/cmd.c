// SPDX-License-Identifier: BSD-3-Clause

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include "cmd.h"
#include "utils.h"

#define READ		0
#define WRITE		1

void redirect(int old_fd, const char *redirect_file, int flag)
{
	int new_fd, rc;

	switch (flag) {
	case O_WRONLY:
		new_fd = open(redirect_file, O_TRUNC | O_WRONLY);
		break;
	case O_APPEND:
		new_fd = open(redirect_file, O_APPEND | O_WRONLY);
		break;
	case O_RDONLY:
		new_fd = open(redirect_file, flag);
		break;
	}

	if (new_fd < 0) {
		// file does not exist
		new_fd = open(redirect_file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
		DIE(new_fd < 0, "open");
	}

	rc = dup2(new_fd, old_fd);
	DIE(rc < 0, "dup2");
	rc = close(new_fd);
	DIE(rc < 0, "close");
}

void restore_fd(int fd, int fd_copy)
{
	int rc;

	rc = dup2(fd_copy, fd);
	DIE(rc < 0, "dup2");
	rc = close(fd_copy);
	DIE(rc < 0, "close");
}

void split_env_command(char *command, char **var, char **value)
{
	char *p = command;

	while (*p != '=')
		p++;

	*p = '\0';

	*var = command;
	*value = p + 1;
}

/**
 * Internal change-directory command.
 */
static bool shell_cd(word_t *dir)
{
	return chdir(dir->string);
}

/**
 * Parse a simple command (internal, environment variable assignment,
 * external command).
 */
static int parse_simple(simple_command_t *s, int level, command_t *father)
{
	int ret, pid, wstatus, stderr_copy, stdout_copy, stdin_copy, args_count = 0;
	char *file_out, *file_err, *file_in, *curr_dir;
	char **args;

	stdin_copy = dup(STDIN_FILENO);
	stdout_copy = dup(STDOUT_FILENO);
	stderr_copy = dup(STDERR_FILENO);

	file_in = get_word(s->in);
	file_out = get_word(s->out);
	file_err = get_word(s->err);
	args = get_argv(s, &args_count);

	if (!strcmp("exit", args[0]) || !strcmp("quit", args[0])) {
		ret = SHELL_EXIT;
		goto exit_func;
	}

	switch (s->io_flags) {
	case 3:
		redirect(STDOUT_FILENO, file_out, O_APPEND);
		redirect(STDERR_FILENO, file_err, O_APPEND);
		break;
	case 2:
		if (s->out)
			redirect(STDOUT_FILENO, file_out, O_WRONLY);
		redirect(STDERR_FILENO, file_err, O_APPEND);
		break;
	case 1:
		if (s->err)
			redirect(STDERR_FILENO, file_err, O_WRONLY);
		redirect(STDOUT_FILENO, file_out, O_APPEND);
		break;
	case 0:
		if (s->out || s->err) {
			if (s->out && s->err && !strcmp(file_out, file_err)) {
				redirect(STDOUT_FILENO, file_out, O_WRONLY);
				redirect(STDOUT_FILENO, file_out, O_APPEND);
				redirect(STDERR_FILENO, file_err, O_APPEND);
				break;
			}
		if (s->out)
			redirect(STDOUT_FILENO, file_out, O_WRONLY);
		if (s->err)
			redirect(STDERR_FILENO, file_err, O_WRONLY);
		}
	}

	if (!strcmp("cd", args[0])) {
		if (s->params == NULL) {
			ret = EXIT_SUCCESS;
			goto exit_func;
		}
		ret = shell_cd(s->params);
		if (ret < 0) {
			fprintf(stderr, "%s\n", strerror(errno));
			fflush(stderr);
			ret = EXIT_FAILURE;
		}
		goto exit_func;
	}

	if (!strcmp("pwd", args[0])) {
		curr_dir = NULL;
		curr_dir = getcwd(curr_dir, 200);
		fprintf(stdout, "%s\n", curr_dir);
		fflush(stdout);
		free(curr_dir);
		ret = EXIT_SUCCESS;
		goto exit_func;
	}

	if (s->verb->next_part && !strcmp("=", s->verb->next_part->string)) {
		char *var, *value;

		split_env_command(args[0], &var, &value);
		ret = setenv(var, value, 1);
		if (ret < 0) {
			fprintf(stderr, "%s\n", strerror(errno));
			fflush(stderr);
			ret = EXIT_FAILURE;
		}
		goto exit_func;
	}

	pid = fork();
	DIE(pid < 0, "fork");

	switch (pid) {
	case 0:
		if (s->in)
			redirect(STDIN_FILENO, file_in, O_RDONLY);

		execvp(args[0], args);
		if (errno == ENOENT) {
			fprintf(stderr, "Execution failed for '%s'\n", args[0]);
			fflush(stderr);
		}
		exit(EXIT_FAILURE);
	default:
		if (s->up->up && s->up->up->op == OP_PIPE && s->up->up->cmd2->scmd == s) {
			ret = pid;
			goto exit_func;
		}
		wait(&wstatus);
		ret = WEXITSTATUS(wstatus);
		break;
	}

exit_func:

	free(file_err);
	free(file_in);
	free(file_out);
	for (int i = 0; i < args_count; i++)
		free(args[i]);
	free(args);

	restore_fd(STDIN_FILENO, stdin_copy);
	restore_fd(STDOUT_FILENO, stdout_copy);
	restore_fd(STDERR_FILENO, stderr_copy);

	return ret;
}

/**
 * Parse and execute a command.
 */
int parse_command(command_t *c, int level, command_t *father)
{
	int rc, ret, pid_first, pid_second, first_wstatus, second_wstatus;
	int fdin_copy, fdout_copy, fderr_copy;
	int pipe_fd[2];

	switch (c->op) {
	case OP_SEQUENTIAL:

		if (c->cmd1) {
			ret = parse_command(c->cmd1, level + 1, c);
			if (ret == SHELL_EXIT)
				return SHELL_EXIT;
		}

		return parse_command(c->cmd2, level + 1, c);

	case OP_PARALLEL:

		pid_first = fork();
		DIE(pid_first < 0, "fork");
		switch (pid_first) {
		case 0:
			ret = parse_command(c->cmd1, level + 1, father);
			exit(ret);

		default:
			pid_second = fork();
			DIE(pid_second < 0, "fork");

			switch (pid_second) {
			case 0:
				ret = parse_command(c->cmd2, level + 1, father);
				exit(ret);
			default:
				waitpid(pid_first, &first_wstatus, 0);
				waitpid(pid_second, &second_wstatus, 0);
			}

			return EXIT_SUCCESS;
		}

	case OP_CONDITIONAL_NZERO:

		if (c->cmd1)
			ret = parse_command(c->cmd1, level + 1, c);

		if (ret == EXIT_FAILURE) {
			ret = parse_command(c->cmd2, level + 1, c);

			return ret;
		}

		return EXIT_SUCCESS;

	case OP_CONDITIONAL_ZERO:

		if (c->cmd1)
			ret = parse_command(c->cmd1, level + 1, c);

		if (ret == EXIT_SUCCESS) {
			ret = parse_command(c->cmd2, level + 1, c);

			return ret;
		}

		return EXIT_FAILURE;

	case OP_PIPE:

		fdin_copy = dup(STDIN_FILENO);
		fdout_copy = dup(STDOUT_FILENO);
		fderr_copy = dup(STDERR_FILENO);

		rc = pipe(pipe_fd);
		DIE(rc < 0, "pipe");

		pid_first = fork();
		DIE(pid_first < 0, "fork");

		switch (pid_first) {
		case 0:
			rc = dup2(pipe_fd[WRITE], STDOUT_FILENO);
			DIE(rc < 0, "dup2");
			rc = close(pipe_fd[READ]);
			DIE(rc < 0, "close");
			rc = close(pipe_fd[WRITE]);
			DIE(rc < 0, "close");

			ret = parse_command(c->cmd1, level + 1, c);

			restore_fd(STDIN_FILENO, fdin_copy);
			restore_fd(STDOUT_FILENO, fdout_copy);
			restore_fd(STDERR_FILENO, fderr_copy);
			exit(ret);

		default:
			rc = dup2(pipe_fd[READ], STDIN_FILENO);
			DIE(rc < 0, "dup2");
			rc = close(pipe_fd[READ]);
			DIE(rc < 0, "close");
			rc = close(pipe_fd[WRITE]);
			DIE(rc < 0, "close");

			pid_second = parse_command(c->cmd2, level + 1, c);
			waitpid(pid_first, &first_wstatus, 0);
			waitpid(pid_second, &second_wstatus, 0);

			restore_fd(STDIN_FILENO, fdin_copy);
			restore_fd(STDOUT_FILENO, fdout_copy);
			restore_fd(STDERR_FILENO, fderr_copy);

			return WEXITSTATUS(second_wstatus);
		}

	case OP_NONE:
		return parse_simple(c->scmd, level, c);

	case OP_DUMMY:
		return EXIT_SUCCESS;
	}
	return EXIT_SUCCESS;
}
