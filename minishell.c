#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include "parser.h"

#define MAX_JOBS 256

typedef struct {
    int id;
    pid_t pid;
    char command[1024];
    char status[1024];
} job_t;

int main() {
    job_t jobs[MAX_JOBS];
    int job_count = 0;
    int next_job_id = 1;

    // Ignoramos las señales SIGINT y SIGQUIT
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    while (1) {
        printf("msh> ");
        fflush(stdout);

        char buff[1024];
        // Leer la línea de entrada del usuario
        if (fgets(buff, sizeof(buff), stdin) == 0) {
            break; // Si se alcanza EOF, salir del bucle principal
        }

        // Tokenizamos la entrada con el parser
        tline *line;
        line  = tokenize(buff);

        if (strcmp(buff, "cd\n") == 0 || strncmp(buff, "cd ", 3) == 0) {

            char *dir = strtok(buff + 3, "\n"); // Extraer el directorio del argumento
            if (dir == NULL) {
                // Si no se proporciona argumento, usar $HOME como destino
                dir = getenv("HOME");
                if (dir == NULL) {
                    fprintf(stderr, "Error: no se pudo obtener el directorio HOME\n");
                }
            }
            // Intentar cambiar al directorio especificado
            if (chdir(dir) == -1) {
                fprintf(stderr, "Error al cambiar de directorio\n");
            }

        } else {
            int input_fd = -1;  // Descriptor de ficher para redirección de entrada
            int output_fd = -1; // Descriptor de fichero para redirección de salida

            // Manejo de redirección de entrada
            if (line->redirect_input != NULL) {
                input_fd = open(line->redirect_input, O_RDONLY);
                if (input_fd == -1) {
                    fprintf(stderr, "Error al abrir el archivo de entrada\n");
                }
            }

            // Manejo de redirección de salida
            if (line->redirect_output != NULL) {
                output_fd = open(line->redirect_output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (output_fd == -1) {
                    fprintf(stderr, "Error al abrir o crear el archivo de salida\n");
                }
            }

            int numcommands = line->ncommands;
            int pipefd[numcommands - 1][2];

            // Creamos los pipes necesarios para los comandos
            for (int i = 0; i < numcommands - 1; i++) {

                if (pipe(pipefd[i]) == -1) {
                    fprintf(stderr, "Error al crear el pipe\n");
                    if (input_fd != -1) {
                        close(input_fd);
                    }
                    if (output_fd != -1) {
                        close(output_fd);
                    }
                    return -1;
                }
            }

            // Ejecutamos los comandos en los procesos hijos
            for (int i = 0; i < numcommands; i++) {
                pid_t pid = fork();

                if (pid == -1) {
                    fprintf(stderr, "Error al crear el proceso hijo\n");
                    return -1;
                }

                if (pid == 0) {
                    // Restauramos el funcionamiento de las señales SIGINT y SIGQUIT para los procesos hijos ejecutados en fg
                    signal(SIGINT, SIG_DFL);
                    signal(SIGQUIT, SIG_DFL);

                    // Si hay redirección de entrada (para el primer mandato)
                    if (input_fd != -1 && i == 0) {
                        dup2(input_fd, STDIN_FILENO); // Redirigimos la entrada estándar del proceso
                        close(input_fd); // cerramos el descriptor de fichero
                    }
                    // Si no es el primer mandato
                    if (i > 0) {
                        dup2(pipefd[i - 1][0], STDIN_FILENO); // redirigimos la entrada desde el pipe anterior
                        close(pipefd[i - 1][0]); // cerramos el extremo del pipe anterio
                    }
                    // Si hay redirección de salida y es el último mandato
                    if (i == numcommands - 1 && output_fd != -1) {
                        dup2(output_fd, STDOUT_FILENO); // redirigimos la salida al archivo
                        close(output_fd); // cerramos el descriptor de fichero
                    } else if (i < numcommands - 1) { // Si no hay redirección de salida
                        dup2(pipefd[i][1], STDOUT_FILENO); // redirigimos la salida al extremo de escritura del pipe actual
                        close(pipefd[i][1]); // cerramos el descriptor de fichero
                    }

                    // Cerramos los extremos de escritura y lectura de los pipes.
                    for (int j = 0; j < numcommands - 1; j++) {
                        close(pipefd[j][0]);
                        close(pipefd[j][1]);
                    }

                    tcommand *cmd = &line->commands[i];
                    execvp(cmd->filename, cmd->argv);
                    fprintf(stderr, "Error al ejecutar el comando %s\n", cmd->filename);
                    return -1;
                }
            }

            // Cerramos los pipes del padre
            for (int i = 0; i < numcommands - 1; i++) {
                close(pipefd[i][0]);
                close(pipefd[i][1]);
            }
            if (input_fd != -1) close(input_fd);
            if (output_fd != -1) close(output_fd);

            // Esperamos a los procesos hijos si se ha ejecutado en fg
            if (line->background == 0) {
                for (int i = 0; i < numcommands; i++) {
                    wait(NULL);
                }
            }
        }
    }

    return 0;
}
