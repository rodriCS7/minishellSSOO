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
    int active; // para comprobar si el mandato sigue activo
} job_t;

job_t jobs[MAX_JOBS];
int job_count = 0;
int next_job_id = 1;

void manejador_hijos(int sig); // Declaración del manejador de SIGCHILD

void fg(char* index); // Función para manejar el paso de comandos de bg a fg


int main() {

    // Ignoramos las señales SIGINT y SIGQUIT
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    // Añadimos un manejador para cuando se reciba la señal SIGCHLD (terminacion de un hijo)
    signal(SIGCHLD, manejador_hijos);

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

        // MANDATOS INTERNOS
        //CD
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

        // JOBS
        } else if (strcmp(buff, "jobs\n") == 0) {
            for (int i = 0; i < job_count; i++) {
                if (jobs[i].active) {
                    printf("[%d]+ %-7s %s\n", jobs[i].id, jobs[i].status, jobs[i].command);
                }
            }
        // FG
        } else if (strcmp(buff, "fg\n") == 0 || strncmp(buff, "fg ", 2) == 0) {

            char *index = strtok(buff + 3, "\n");
            fg(index);
        }

        // MANDATOS QUE NO SON INTERNOS

        else {
            
            int input_fd = -1;  // Descriptor de ficher para redirección de entrada
            int output_fd = -1; // Descriptor de fichero para redirección de salida
            int error_fd = -1; // Descriptor de fichero para redirección de error

            // Manejo de redirección de entrada
            if (line->redirect_input != NULL && strlen(line->redirect_input) > 0) {
                printf("%s", line->redirect_input);
                input_fd = open(line->redirect_input, O_RDONLY);
                if (input_fd == -1 ) {
                    fprintf(stderr, "fichero: Error al abrir el archivo de entrada (%s)\n", line->redirect_input);
                    continue;
                }
            }

            // Manejo de redirección de salida
            if (line->redirect_output != NULL && strlen(line->redirect_output) > 0) {
                printf("%s", line->redirect_output);
                output_fd = open(line->redirect_output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (output_fd == -1) {
                    fprintf(stderr, "fichero: Error al abrir o crear el archivo de salida (%s)\n", line->redirect_output);
                    continue;                
                }
            }
            
            // Manejo de redirección de error
            if (line->redirect_error != NULL && strlen(line->redirect_error) > 0) {
                printf("%s", line->redirect_error);
                error_fd = open(line->redirect_error, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (error_fd == -1) {
                    fprintf(stderr, "fichero: Error al abrir o crear el archivo para redireccón de error (%s)\n", line->redirect_error);
                    continue;                
                }
            }

            int numcommands = line->ncommands;
            int pipefd[numcommands - 1][2];

            // pipe[X][1] --> entrada / escritura
            // pipe[X][0] --> salida / lectura

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
                    if (error_fd != -1) {
                        close(error_fd);
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
                    if (line->background == 0) {
                        signal(SIGINT, SIG_DFL);
                        signal(SIGQUIT, SIG_DFL);
                    }

                    // Si hay redirección de entrada (para el primer mandato)
                    if (input_fd != -1 && i == 0) {
                        dup2(input_fd, STDIN_FILENO); // Redirigimos la entrada estándar del proceso
                        close(input_fd); // cerramos el descriptor de fichero
                    }
                    // Si no es el primer mandato
                    if (i > 0) {
                        dup2(pipefd[i - 1][0], STDIN_FILENO); // redirigimos la entrada desde el pipe anterior
                        close(pipefd[i - 1][0]); // cerramos el extremo del pipe anterior
                    }
                    // Si es el último mandato comprobamos si hay redireccion de salida o error
                    if (i == numcommands - 1) {
                        if (output_fd != -1) {
                            dup2(output_fd, STDOUT_FILENO); // redirigimos la salida al archivo
                            close(output_fd); // cerramos el descriptor de fichero
                        }
                        if (error_fd != -1) {
                            dup2(error_fd, STDERR_FILENO); // redirigimos STDERR al archivo
                            close(error_fd); // Cerramos el descriptor de fichero
                        }

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

                    // El parser devuelve NULL si no existe el mandato(filename)
                    if (cmd->filename == NULL) {
                        printf("%s: No se encuentra el mandato\n", cmd->argv[0]);
                        return -1;
                    }       

                    execvp(cmd->filename, cmd->argv);
                    fprintf(stderr, "Error al ejecutar el comando %s\n", cmd->filename);
                    return -1;

                } else { // No somos el hijo
                    // Añadimos el comando al array de jobs
                    if (line->background == 1 && job_count < MAX_JOBS) {
                        
                        jobs[job_count].id = next_job_id++; // Asignamos un id al comando actual y lo incrementamos
                        jobs[job_count].pid = pid; // asignamos el pid del proceso hijo
                        jobs[job_count].active = 1; // el proceso pasa a estar activo
                        strncpy(jobs[job_count].command, line->commands[i].filename, sizeof(jobs[job_count].command)); // Asignamos el comando
                        strncpy(jobs[job_count].status, "Running", sizeof(jobs[job_count].status)); // Estado actual
                        job_count++;
                        
                    }
                }
            }

            // Cerramos los pipes del padre
            for (int i = 0; i < numcommands - 1; i++) {
                close(pipefd[i][0]);
                close(pipefd[i][1]);
            }
            if (input_fd != -1) {
                close(input_fd);
            }
            if (output_fd != -1) {
                close(output_fd);
            }
            if (error_fd != -1) {
                close(error_fd);
            }

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

void manejador_hijos(int signo) {
    pid_t pid;
    int status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) { // WNOHANG testea si algún hijo ha terminado
        for (int i = 0; i < job_count; i++) {
            if (jobs[i].pid == pid) {
                jobs[i].active = 0; // El proceso ya no está activo
                if (WIFEXITED(status)) { // !=0 si el hijo ha terminado
                    strncpy(jobs[i].status, "Done", sizeof(jobs[i].status));
                }
                break;
            }
        }
    }
}

void fg(char* index) {

    int job_id = -1;
    int target_index = -1;
    int existe = 0;

    // Si se proporciona un ID de job, buscamos el job correspondiente
    if (index != NULL) {
        job_id = atoi(index);
        for (int i = 0; i < job_count; i++) {
            if (jobs[i].id == job_id && jobs[i].active) {
                target_index = i;
                existe = 1;
                break;
            }
        }
    } else {
        // Si no se proporciona ID, usamos el último trabajo activo
        for (int i = 0; i < job_count; i++) {
            if (jobs[i].active) {
                target_index = i;
                job_id = jobs[i].id;
                existe = 1;
                break;
            }
        }
    }

    if (!existe || target_index == -1) {
        fprintf(stderr, "fg: No existe un trabajo activo con ese ID\n");
        return;
    }


    job_t *job = &jobs[target_index];
    printf("Reanudando proceso [%d] %s\n", job->id, job->command);

    waitpid(jobs[job_id].pid, NULL, 0);

    jobs[job_id].active = 0;
    strncpy(job->status, "Done", sizeof(job->status)); 
}

