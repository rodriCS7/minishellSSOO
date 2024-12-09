#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include "parser.h"
#include <signal.h>
#include <string.h>

int main()
{

	// Ignorar la señal SIGINT en el proceso padre
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

		// Tokenizamos la línea ingresada
		tline *line;
		line = tokenize(buff);

		// Manejo del mandato cd
		// el parser devuelve NULL en filename, por lo que debemos manejar los mandatos internos a parte
		// fgets para obtener el buffer mete un \n al final, por eso hay que comparar con "cd\n"
		if (strcmp(buff, "cd\n") == 0 || strncmp(buff, "cd ", 3) == 0) 
		{
			char *dir = strtok(buff + 3, "\n");
			if (dir == NULL)
			{
				// Sin argumentos cambiamos a $HOME
				dir = getenv("HOME");
				if (dir == NULL)
				{
					//dir= "/home/rodrics"; // Reemplazar con la ruta a $HOME
					fprintf(stderr, "Error: no se pudo obtener el directorio HOME");
				}
			}
			if (chdir(dir) == -1)
			{
				fprintf(stderr, "Error al cambiar de directorio");
			}
		}
		// En caso de que no sea cd el/los mandatos introducidos
		else
		{

			int input_fd = -1;	// Descriptor de fichero para redirección de entrada
			int output_fd = -1; // Descriptor de fichero para redirección de salida

			// Redirigir la entrada estandar
			if (line->redirect_input != NULL)
			{
				input_fd = open(line->redirect_input, O_RDONLY);
				if (input_fd == -1)
				{
					fprintf(stderr, "Error al abrir el archivo de entrada");
				}
			}

			// Redirigir la salida estándar
			if (line->redirect_output != NULL)
			{
				output_fd = open(line->redirect_output, O_WRONLY | O_CREAT | O_TRUNC);
				if (output_fd == -1)
				{
					fprintf(stderr, "Error al abrir o crear el fichero de salida");
				}
			}

			if (line->ncommands == 1)
			{
				pid_t pid = fork();

				// Si se ha producido un error al hacer el fork
				if (pid == -1)
				{
					if (input_fd != -1)
					{
						close(input_fd);
					}
					if (output_fd != -1)
					{
						close(output_fd);
					}
				}

				// Si somos el hijo
				if (pid == 0)
				{

					// Restauramos el comportamiento de la señal SIGINT
					signal(SIGINT, SIG_DFL);

					// redirigimos la entrada estándar
					if (input_fd != -1)
					{
						dup2(input_fd, STDIN_FILENO);
						close(input_fd);
					}
					// redirigimos la salida estándar
					if (output_fd != -1)
					{
						dup2(output_fd, STDOUT_FILENO);
						close(output_fd);
					}

					// Ejecutamos el comando
					execvp(line->commands[0].filename, line->commands[0].argv);

					// En caso de error
					fprintf(stderr, "Error al ejecutar el mandato");
					return -1;
				}

				// Si somos el padre
				if (input_fd != -1)
				{
					close(input_fd);
				}
				if (output_fd != -1)
				{
					close(output_fd);
				}
				waitpid(pid, NULL, 0);
			}
			else if (line->ncommands == 2)
			{

				int pipefd[2];

				// Si se produce un error al crear el pipe
				if (pipe(pipefd) == -1)
				{
					fprintf(stderr, "Error al crear el pipe\n");
					if (input_fd != -1)
					{
						close(input_fd);
					}
					if (output_fd != -1)
					{
						close(output_fd);
					}
				}

				pid_t pid1 = fork();
				if (pid1 == -1)
				{
					fprintf(stderr, "Error al hacer el fork para el primer hijo\n");
					close(pipefd[0]);
					close(pipefd[1]);
					if (input_fd != -1)
						close(input_fd);
					if (output_fd != -1)
						close(output_fd);
				}

				// Somos el pimer hijo
				if (pid1 == 0)
				{

					// Restauramos el funcionamiento de la señal SIGINT
					signal(SIGINT, SIG_DFL);

					// Redirigmos la entrada estandar
					if (input_fd != -1)
					{
						dup2(input_fd, STDIN_FILENO);
						close(input_fd);
					}

					close(pipefd[0]);				// Cerramos el extremo de lectura
					dup2(pipefd[1], STDOUT_FILENO); // Redirigimos stdout al extremo de escritura del pipe
					close(pipefd[1]);				// Cerramos el extremo de escritura

					tcommand *cmd1 = &line->commands[0];
					execvp(cmd1->filename, cmd1->argv);
					fprintf(stderr, "Error al ejecutar el primer mandato\n");
				}

				pid_t pid2 = fork();
				if (pid2 == -1)
				{
					fprintf(stderr, "Error al hacer el fork para el segundo hijo\n");
					close(pipefd[0]);
					close(pipefd[1]);
					if (input_fd != -1)
						close(input_fd);
					if (output_fd != -1)
						close(output_fd);
				}

				// Somos el segundo hijo
				if (pid2 == 0)
				{

					// Restauramos el funcionamiento de la señal SIGINT
					signal(SIGINT, SIG_DFL);

					if (output_fd != -1)
					{
						dup2(output_fd, STDOUT_FILENO);
						close(output_fd);
					}

					close(pipefd[1]);			   // Cerramos el extremo de escritura del pipe
					dup2(pipefd[0], STDIN_FILENO); // Redirigimos la entrada estándar al pipe
					close(pipefd[0]);

					tcommand *cmd2 = &line->commands[1];
					execvp(cmd2->filename, cmd2->argv);
					fprintf(stderr, "Error al ejecutar el segundo mandato\n");
				}

				// Código del padre
				close(pipefd[0]);
				close(pipefd[1]);
				if (input_fd != -1)
				{
					close(input_fd);
				}
				if (output_fd != -1)
				{
					close(output_fd);
				}

				waitpid(pid1, NULL, 0);
				waitpid(pid2, NULL, 0);
			}
		}
	}

	return 0;
}
