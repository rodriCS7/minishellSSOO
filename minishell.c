#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "parser.h"
#include <signal.h>

int main()
{

	signal(SIGINT, SIG_IGN);

	while (1)
	{

		printf("msh> ");
		fflush(stdout);

		char buff[1024];
		if (fgets(buff, sizeof(buff), stdin) == 0)
		{
			break;
		}

		tline *line;
		line = tokenize(buff);

		if (line->ncommands == 1)
		{
			pid_t pid = fork();
			if (pid == 0) {
				signal(SIGINT, SIG_DFL);

				if (execvp(line->commands[0].filename, line->commands[0].argv) == -1) {
					fprintf(stderr, "Error al ejecutar el mandato");
				}
			}

			waitpid(pid, NULL, 0);
		}
		else
		{

			int pipefd[2];

			if (pipe(pipefd) == -1)
			{
				fprintf(stderr, "Error al crear el pipe\n");
			}

			pid_t pid1 = fork();
			if (pid1 == -1)
			{
				fprintf(stderr, "Error al hacer el fork para el primer hijo\n");
			}

			// Somos el pimer hijo
			if (pid1 == 0)
			{
				signal(SIGINT, SIG_DFL);
				close(pipefd[0]);				// Cerramos el extremo de lectura
				dup2(pipefd[1], STDOUT_FILENO); // Redirigimos stdout al extremos de escritura del pipe
				close(pipefd[1]);				// Cerramos el extremo de escritura

				tcommand *cmd1 = &line->commands[0];
				execvp(cmd1->filename, cmd1->argv);
				fprintf(stderr, "Error al ejecutar el primer mandato\n");
			}

			pid_t pid2 = fork();
			if (pid2 == -1)
			{
				fprintf(stderr, "Error al hacer el fork para el segundo hijo\n");
			}

			// Somos el segundo hijo
			if (pid2 == 0)
			{
				signal(SIGINT, SIG_DFL);
				close(pipefd[1]);
				dup2(pipefd[0], STDIN_FILENO);
				close(pipefd[0]);

				tcommand *cmd2 = &line->commands[1];
				execvp(cmd2->filename, cmd2->argv);
				fprintf(stderr, "Error al ejecutar el segundo mandato\n");
			}

			// Código del padre
			close(pipefd[0]);
			close(pipefd[1]);
			waitpid(pid1, NULL, 0);
			waitpid(pid2, NULL, 0);
		}
	}

	return 0;
}
