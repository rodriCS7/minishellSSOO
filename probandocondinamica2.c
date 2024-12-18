#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include "parser.h"

typedef struct {
    int id;
    pid_t pid;
    char command[1024];
    char status[1024];
    int active;
} job_t;

job_t *jobs = NULL; // Array dinámico de trabajos
int job_count = 0;  // Contador de trabajos
int next_job_id = 1;

// Manejador de señal SIGCHLD
void manejador_hijos(int signo) {
    pid_t pid;
    int status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < job_count; i++) {
            if (jobs[i].pid == pid) {
                jobs[i].active = 0;
                strncpy(jobs[i].status, "Done", sizeof(jobs[i].status));
                break;
            }
        }
    }
}

void fg(char* index) {
    int job_id = -1, target_index = -1;

    // Buscar el trabajo
    if (index != NULL) {
        job_id = atoi(index);
        for (int i = 0; i < job_count; i++) {
            if (jobs[i].id == job_id && jobs[i].active) {
                target_index = i;
                break;
            }
        }
    }

    if (target_index == -1) {
        fprintf(stderr, "fg: No existe un trabajo activo con ese ID\n");
        return;
    }

    printf("Reanudando proceso [%d] %s\n", jobs[target_index].id, jobs[target_index].command);
    waitpid(jobs[target_index].pid, NULL, 0);
    jobs[target_index].active = 0;
    strncpy(jobs[target_index].status, "Done", sizeof(jobs[target_index].status));
}

int main() {
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGCHLD, manejador_hijos);

    while (1) {
        printf("msh> ");
        fflush(stdout);

        char buff[1024];
        if (fgets(buff, sizeof(buff), stdin) == NULL) {
            break;
        }

        // Eliminar espacios y saltos de línea del comando
        char *command_trimmed = strtok(buff, "\n");
        if (command_trimmed == NULL || strlen(command_trimmed) == 0) {
            continue; // Comando vacío
        }

        tline *line = tokenize(buff);
        if (line == NULL) continue;

        if (strcmp(buff, "cd\n") == 0 || strncmp(buff, "cd ", 3) == 0) {
            char *dir = strtok(buff + 3, "\n");
            if (dir == NULL) dir = getenv("HOME");
            if (chdir(dir) == -1) fprintf(stderr, "Error al cambiar de directorio\n");
        } else if (strcmp(buff, "jobs\n") == 0) {
            for (int i = 0; i < job_count; i++) {
                if (jobs[i].active) {
                    printf("[%d]+ %-7s %s\n", jobs[i].id, jobs[i].status, jobs[i].command);
                }
            }
        } else if (strncmp(buff, "fg", 2) == 0) {
            char *index = strtok(buff + 3, "\n");
            fg(index);
        } else {
            int input_fd = -1, output_fd = -1, error_fd = -1;

            // **Redirección de entrada**
            if (line->redirect_input) {
                input_fd = open(line->redirect_input, O_RDONLY);
                if (input_fd == -1) {
                    fprintf(stderr, "Error al abrir el archivo de entrada: %s\n", line->redirect_input);
                    continue;
                }
            }

            // **Redirección de salida**
            if (line->redirect_output) {
                output_fd = open(line->redirect_output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (output_fd == -1) {
                    fprintf(stderr, "Error al abrir el archivo de salida: %s\n", line->redirect_output);
                    if (input_fd != -1) close(input_fd);
                    continue;
                }
            }

            // **Redirección de error**
            if (line->redirect_error) {
                error_fd = open(line->redirect_error, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (error_fd == -1) {
                    fprintf(stderr, "Error al abrir el archivo de error: %s\n", line->redirect_error);
                    if (input_fd != -1) close(input_fd);
                    if (output_fd != -1) close(output_fd);
                    continue;
                }
            }

            pid_t pid = fork();
            if (pid == 0) {
                // Restaurar señales en procesos hijos
                signal(SIGINT, SIG_DFL);
                signal(SIGQUIT, SIG_DFL);

                if (input_fd != -1) {
                    dup2(input_fd, STDIN_FILENO);
                    close(input_fd);
                }
                if (output_fd != -1) {
                    dup2(output_fd, STDOUT_FILENO);
                    close(output_fd);
                }
                if (error_fd != -1) {
                    dup2(error_fd, STDERR_FILENO);
                    close(error_fd);
                }

                tcommand *cmd = &line->commands[0];
                execv(cmd->filename, cmd->argv);
                fprintf(stderr, "Error al ejecutar el comando %s\n", cmd->filename);
                exit(1);
            } else if (pid > 0) {
                if (line->background) {
                    // Realocar dinámicamente la lista de trabajos
                    jobs = realloc(jobs, (job_count + 1) * sizeof(job_t));
                    jobs[job_count].id = next_job_id++;
                    jobs[job_count].pid = pid;
                    jobs[job_count].active = 1;
                    strncpy(jobs[job_count].command, line->commands[0].filename, sizeof(jobs[job_count].command));
                    strncpy(jobs[job_count].status, "Running", sizeof(jobs[job_count].status));
                    job_count++;
                } else {
                    waitpid(pid, NULL, 0);
                }
            } else {
                fprintf(stderr, "Error al crear el proceso hijo\n");
            }

            if (input_fd != -1) close(input_fd);
            if (output_fd != -1) close(output_fd);
            if (error_fd != -1) close(error_fd);
        }
    }

    // Liberar memoria al finalizar
    free(jobs);
    return 0;
}
