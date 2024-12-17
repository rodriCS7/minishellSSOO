#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include "parser.h"
#include <signal.h>
#include <string.h>

#define MAX_JOBS 256

typedef struct {
    int id;
    pid_t pid;
    char command[1024];
    char status[1024];
} job_t;

int main() {

    // Definimos un array para almacenar los comandos ejecutados en bg

    job_t jobs[MAX_JOBS];
    int job_count = 0;  // número actual mandatos ejecutados en bg
    int next_job_id = 1;  // ID para el siguiente mandato

    // Ignorar la señal SIGINT en el proceso principal para evitar que el shell termine por interrupciones
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    while (1)
    {
        // Imprimir el prompt del shell
        printf("msh> ");
        fflush(stdout);

        char buff[1024];
        // Leer la línea de entrada del usuario
        if (fgets(buff, sizeof(buff), stdin) == 0)
        {
            break; // Si se alcanza EOF, salir del bucle principal
        }

        // Tokenizar la línea ingresada usando una función de biblioteca externa (parser.h)
        tline *line;
        line = tokenize(buff);

        // Manejo del comando "cd" (cambiar de directorio)
        if (strcmp(buff, "cd\n") == 0 || strncmp(buff, "cd ", 3) == 0)
        {
            char *dir = strtok(buff + 3, "\n"); // Extraer el directorio del argumento
            if (dir == NULL)
            {
                // Si no se proporciona argumento, usar $HOME como destino
                dir = getenv("HOME");
                if (dir == NULL)
                {
                    fprintf(stderr, "Error: no se pudo obtener el directorio HOME\n");
                }
            }
            // Intentar cambiar al directorio especificado
            if (chdir(dir) == -1)
            {
                fprintf(stderr, "Error al cambiar de directorio\n");
            }
        }
        else // Comandos que no son "cd"
        {
            int input_fd = -1;  // Descriptor de archivo para redirección de entrada
            int output_fd = -1; // Descriptor de archivo para redirección de salida

            // Manejo de redirección de entrada
            if (line->redirect_input != NULL)
            {
                input_fd = open(line->redirect_input, O_RDONLY);
                if (input_fd == -1)
                {
                    fprintf(stderr, "Error al abrir el archivo de entrada\n");
                }
            }

            // Manejo de redirección de salida
            if (line->redirect_output != NULL)
            {
                output_fd = open(line->redirect_output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (output_fd == -1)
                {
                    fprintf(stderr, "Error al abrir o crear el archivo de salida\n");
                }
            }

            // Si hay un solo comando en la línea
            if (line->ncommands == 1)
            {
                pid_t pid = fork(); // Crear un proceso hijo

                if (pid == -1)
                {
                    fprintf(stderr, "Error al crear el proceso hijo\n");
					// Cerramos los descriptores de fichero en caso de que estuvieran abiertos
                    if (input_fd != -1)
                        close(input_fd);
                    if (output_fd != -1)
                        close(output_fd);
                }

                if (pid == 0) // Código del hijo
                {
                    // Restaurar comportamiento predeterminado para SIGINT y SIGQUIT
                    signal(SIGINT, SIG_DFL);
                    signal(SIGQUIT, SIG_DFL);

                    // Redirigir la entrada estándar si es necesario
                    if (input_fd != -1)
                    {
                        dup2(input_fd, STDIN_FILENO);
                        close(input_fd);
                    }

                    // Redirigir la salida estándar si es necesario
                    if (output_fd != -1)
                    {
                        dup2(output_fd, STDOUT_FILENO);
                        close(output_fd);
                    }

                    // Ejecutar el comando
                    execvp(line->commands[0].filename, line->commands[0].argv);

                    // Si execvp falla
                    fprintf(stderr, "Error al ejecutar el comando\n");
                    return -1;
                }

                // Código del padre: cerrar descriptores y esperar al hijo
                if (input_fd != -1)
                    close(input_fd);
                if (output_fd != -1)
                    close(output_fd);
                
                if (line->background == 0) {
                    waitpid(pid, NULL, 0);
                }
            }
            else if (line->ncommands == 2) // Si hay dos comandos (pipe simple)
            {
                int pipefd[2]; // Crear un pipe

                if (pipe(pipefd) == -1)
                {
                    fprintf(stderr, "Error al crear el pipe\n");
                    if (input_fd != -1)
                        close(input_fd);
                    if (output_fd != -1)
                        close(output_fd);
                }

                pid_t pid1 = fork();
                if (pid1 == -1)
                {
                    fprintf(stderr, "Error al crear el primer proceso hijo\n");
                    close(pipefd[0]);
                    close(pipefd[1]);
                    if (input_fd != -1)
                        close(input_fd);
                    if (output_fd != -1)
                        close(output_fd);
                }

                if (pid1 == 0) // Primer hijo
                {
                    signal(SIGINT, SIG_DFL);
                    signal(SIGQUIT, SIG_DFL);


                    // Redirigir entrada estándar
                    if (input_fd != -1)
                    {
                        dup2(input_fd, STDIN_FILENO);
                        close(input_fd);
                    }

                    // Redirigir salida al extremo de escritura del pipe
                    close(pipefd[0]);
                    dup2(pipefd[1], STDOUT_FILENO);
                    close(pipefd[1]);

                    tcommand *cmd1 = &line->commands[0];
                    execvp(cmd1->filename, cmd1->argv);

                    fprintf(stderr, "Error al ejecutar el primer comando\n");
                }

                pid_t pid2 = fork();
                if (pid2 == -1)
                {
                    fprintf(stderr, "Error al crear el segundo proceso hijo\n");
                    close(pipefd[0]);
                    close(pipefd[1]);
                    if (input_fd != -1)
                        close(input_fd);
                    if (output_fd != -1)
                        close(output_fd);
                }

                if (pid2 == 0) // Segundo hijo
                {
                    signal(SIGINT, SIG_DFL);
                    signal(SIGQUIT, SIG_DFL);

                    // Redirigir salida estándar
                    if (output_fd != -1)
                    {
                        dup2(output_fd, STDOUT_FILENO);
                        close(output_fd);
                    }

                    // Redirigir entrada al extremo de lectura del pipe
                    close(pipefd[1]);
                    dup2(pipefd[0], STDIN_FILENO);
                    close(pipefd[0]);

                    tcommand *cmd2 = &line->commands[1];
                    execvp(cmd2->filename, cmd2->argv);

                    fprintf(stderr, "Error al ejecutar el segundo comando\n");
                }

                // Código del padre: cerrar extremos del pipe y esperar a los hijos
                close(pipefd[0]);
                close(pipefd[1]);
                if (input_fd != -1)
                    close(input_fd);
                if (output_fd != -1)
                    close(output_fd);

                if (line->background == 0) {
                    waitpid(pid1, NULL, 0);
                    waitpid(pid2, NULL, 0);
                }
                
            }
            else if (line->ncommands > 2) // Para más de dos comandos (pipes múltiples)
            {
                int numcommands = line->ncommands;
                int pipefd[numcommands - 1][2]; // Crear un array de pipes

                // Crear pipes
                for (int i = 0; i < numcommands - 1; i++)
                {
                    if (pipe(pipefd[i]) == -1) {

                    	if (input_fd != -1) {
                        	close(input_fd);
						}
                    	if (output_fd != -1) {
                        	close(output_fd);
						}
                        fprintf(stderr, "Error al crear el pipe\n");

                    }

                }

                for (int i = 0; i < numcommands; i++)
                {
                    pid_t pid = fork();

                    if (pid == -1)
                    {
                        fprintf(stderr, "Error al crear el proceso hijo\n");
                    }

                    if (pid == 0) // Código del hijo
                    {
                        signal(SIGINT, SIG_DFL);
                        signal(SIGQUIT, SIG_DFL);

						// Redirigimos la entrada desde fichero para el primer mandato
						if (i == 0 && input_fd != -1) {
							dup2(input_fd, STDIN_FILENO);
						}

                        // Redirigir entrada si no es el primer comando
                        if (i > 0) {
                            dup2(pipefd[i - 1][0], STDIN_FILENO);
                            close(pipefd[i - 1][0]);
                        }

						if (i == numcommands - 1 && output_fd != -1) {
							dup2(output_fd, STDOUT_FILENO);
						}

                        // Redirigir salida si no es el último comando
                        if (i < numcommands - 1)
                        {
                            dup2(pipefd[i][1], STDOUT_FILENO);
                            close(pipefd[i][1]);
                        }

                        // Cerrar extremos innecesarios del pipe
                        for (int j = 0; j < numcommands - 1; j++)
                        {
                            close(pipefd[j][0]);
                            close(pipefd[j][1]);
                        }

                        tcommand *cmd = &line->commands[i];
                        execvp(cmd->filename, cmd->argv);
                        fprintf(stderr, "Error al ejecutar el comando %s\n", cmd->filename);
                    }
                }

                // Código del padre: cerrar todos los extremos del pipe
                for (int i = 0; i < numcommands - 1; i++)
                {
                    close(pipefd[i][0]);
                    close(pipefd[i][1]);
                }
				// Y los descriptores de ficheros
				if (input_fd != -1) {
					close(input_fd);
				}
				if (output_fd != -1) {
					close(output_fd);
				}

                // Esperar a todos los hijos
                if (line->background == 0) {
                    for (int i = 0; i < numcommands; i++)
                    {
                        wait(NULL);
                    }
                }
            }
        }
    }

    return 0;
}
