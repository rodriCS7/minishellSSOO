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

// Estructura que representa un job
typedef struct {
    int id; // ID del job
    pid_t pid; // PID del proceso asociado al job
    char comando[1024]; // Comando asociado al job
    char estado[1024]; // Estado del job ("Running", "Done", etc.)
    int activo; // Indica si el job sigue activo
} job_t;

job_t jobs[MAX_JOBS]; // Array que almacena los jobs activos
int contador_jobs = 0; // Contador de jobs actuales
int siguiente_id_job = 1; // ID que se asignará al próximo job

void manejador_hijos(int sig); // Declaración del manejador de SIGCHILD

void fg(char* indice); // Función para manejar el paso de comandos de background (bg) a foreground (fg)

int main() {

    // Ignoramos las señales SIGINT y SIGQUIT
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    // Añadimos un manejador para cuando se reciba la señal SIGCHLD (terminación de un proceso hijo)
    signal(SIGCHLD, manejador_hijos);

    while (1) {
        printf("msh> ");
        fflush(stdout);

        char buffer[1024];
        // Leer la línea de entrada del usuario
        if (fgets(buffer, sizeof(buffer), stdin) == 0) {
            break; // Si se alcanza EOF, salir del bucle principal
        }

        // Tokenizamos la entrada con el parser
        tline *linea;
        linea = tokenize(buffer);

        // COMANDOS INTERNOS
        // Cambiar de directorio (cd)
        if (strcmp(buffer, "cd\n") == 0 || strncmp(buffer, "cd ", 3) == 0) {

            char *directorio = strtok(buffer + 3, "\n"); // Extraer el directorio del argumento
            if (directorio == NULL) {
                // Si no se proporciona argumento, usar $HOME como destino
                directorio = getenv("HOME");
                if (directorio == NULL) {
                    fprintf(stderr, "Error: no se pudo obtener el directorio HOME\n");
                }
            }
            // Intentar cambiar al directorio especificado
            if (chdir(directorio) == -1) {
                fprintf(stderr, "Error al cambiar de directorio\n");
            }

        // Mostrar jobs activos (jobs)
        } else if (strcmp(buffer, "jobs\n") == 0) {
            for (int i = 0; i < contador_jobs; i++) {
                if (jobs[i].activo) {
                    printf("[%d]+ %-7s %s\n", jobs[i].id, jobs[i].estado, jobs[i].comando);
                }
            }
        // Mover un job a foreground (fg)
        } else if (strcmp(buffer, "fg\n") == 0 || strncmp(buffer, "fg ", 2) == 0) {
            char *indice = strtok(buffer + 3, "\n");
            fg(indice);
        }

        // COMANDOS EXTERNOS
        else {
            
            int entrada_fd = -1;  // Descriptor de fichero para redirección de entrada
            int salida_fd = -1; // Descriptor de fichero para redirección de salida
            int error_fd = -1; // Descriptor de fichero para redirección de error

            if (linea != NULL) {
            // Manejo de redirección de entrada
            if (linea->redirect_input) {
                entrada_fd = open(linea->redirect_input, O_RDONLY);
                if (entrada_fd == -1 ) {
                    fprintf(stderr, "Error al abrir el archivo de entrada (%s)\n", linea->redirect_input);
                    return -1;
                }
            }

            // Manejo de redirección de salida
            if (linea->redirect_output) {
                salida_fd = open(linea->redirect_output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (salida_fd == -1) {
                    fprintf(stderr, "Error al abrir o crear el archivo de salida (%s)\n", linea->redirect_output);
                    return -1;                
                }
            }
            
            // Manejo de redirección de error
            if (linea->redirect_error) {
                error_fd = open(linea->redirect_error, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (error_fd == -1) {
                    fprintf(stderr, "Error al abrir o crear el archivo para redirección de error (%s)\n", linea->redirect_error);
                    return -1;                
                }
            }

            int num_comandos = linea->ncommands;

            // int pipefd[num_comandos - 1][2];
            // Pipes con memoria dinámica

            int **pipefd = malloc((num_comandos - 1) * sizeof(int *));
            if (pipefd == NULL) {
                fprintf(stderr, "Error al reservar memoria para los pipes\n");
                return -1;
            }

            // Reservamos memoria para cada pipe (dos enteros: lectura y escritura)
            for (int i = 0; i < num_comandos - 1; i++) {
                pipefd[i] = malloc(2 * sizeof(int));
                if (pipefd[i] == NULL) {
                    fprintf(stderr, "Error al reservar memoria para el pipe %d\n", i);
                    // Liberamos la memoria previamente asignada
                    for (int j = 0; j < i; j++) {
                        free(pipefd[j]);
                    }
                    free(pipefd);
                    return -1;
                }

                // Crea el pipe
                if (pipe(pipefd[i]) == -1) {
                    fprintf(stderr, "Error al crear el pipe %d\n", i);
                    // Liberamos la memoria asignada
                    for (int j = 0; j <= i; j++) {
                        free(pipefd[j]);
                    }
                    free(pipefd);
                    if (entrada_fd != -1) {
                        close(entrada_fd);
                    }
                    if (salida_fd != -1) {
                        close(salida_fd);
                    }
                    if (error_fd != -1) {
                        close(error_fd);
                    }
                    return -1;
                    return -1;
                }
            }
            // pipe[X][1] --> entrada / escritura
            // pipe[X][0] --> salida / lectura

            // Ejecutamos los comandos en los procesos hijos
            for (int i = 0; i < num_comandos; i++) {
                pid_t pid = fork();

                if (pid == -1) {
                    fprintf(stderr, "Error al crear el proceso hijo\n");
                    return -1;
                }

                if (pid == 0) {
                    // Restauramos el funcionamiento de las señales SIGINT y SIGQUIT para los procesos hijos ejecutados en fg
                    if (linea->background == 0) {
                        signal(SIGINT, SIG_DFL);
                        signal(SIGQUIT, SIG_DFL);
                    }

                    // Si hay redirección de entrada (para el primer comando)
                    if (entrada_fd != -1 && i == 0) {
                        dup2(entrada_fd, STDIN_FILENO); // Redirigimos la entrada estándar del proceso
                        close(entrada_fd); // cerramos el descriptor de fichero
                    }
                    // Si no es el primer comando
                    if (i > 0) {
                        dup2(pipefd[i - 1][0], STDIN_FILENO); // redirigimos la entrada desde el pipe anterior
                        close(pipefd[i - 1][0]); // cerramos el extremo del pipe anterior
                    }
                    // Si es el último comando comprobamos si hay redirección de salida o error
                    if (i == num_comandos - 1) {
                        if (salida_fd != -1) {
                            dup2(salida_fd, STDOUT_FILENO); // redirigimos la salida al archivo
                            close(salida_fd); // cerramos el descriptor de fichero
                        }
                        if (error_fd != -1) {
                            dup2(error_fd, STDERR_FILENO); // redirigimos STDERR al archivo
                            close(error_fd); // Cerramos el descriptor de fichero
                        }

                    } else if (i < num_comandos - 1) { // Si no hay redirección de salida
                        dup2(pipefd[i][1], STDOUT_FILENO); // redirigimos la salida al extremo de escritura del pipe actual
                        close(pipefd[i][1]); // cerramos el descriptor de fichero
                    }

                    // Cerramos los extremos de escritura y lectura de los pipes.
                    for (int j = 0; j < num_comandos - 1; j++) {
                        close(pipefd[j][0]);
                        close(pipefd[j][1]);
                    }

                    tcommand *cmd = &linea->commands[i];

                    // El parser devuelve NULL si no existe el comando (filename)
                    if (cmd->filename == NULL) {
                        printf("%s: No se encuentra el comando\n", cmd->argv[0]);
                        return -1;
                    }       

                    execv(cmd->filename, cmd->argv);
                    fprintf(stderr, "Error al ejecutar el comando %s\n", cmd->filename);
                    return -1;

                } else { // No somos el hijo
                    // Añadimos el comando al array de jobs
                    if (linea->background == 1 && contador_jobs < MAX_JOBS) {
                        
                        jobs[contador_jobs].id = siguiente_id_job++; // Asignamos un id al comando actual y lo incrementamos
                        jobs[contador_jobs].pid = pid; // asignamos el pid del proceso hijo
                        jobs[contador_jobs].activo = 1; // el proceso pasa a estar activo
                        strncpy(jobs[contador_jobs].comando, linea->commands[i].filename, sizeof(jobs[contador_jobs].comando)); // Asignamos el comando
                        strncpy(jobs[contador_jobs].estado, "Running", sizeof(jobs[contador_jobs].estado)); // Estado actual
                        contador_jobs++;
                        
                    }
                }
            }

            // Cerramos los pipes del padre
            for (int i = 0; i < num_comandos - 1; i++) {
                close(pipefd[i][0]);
                close(pipefd[i][1]);
            }
            if (entrada_fd != -1) {
                close(entrada_fd);
            }
            if (salida_fd != -1) {
                close(salida_fd);
            }
            if (error_fd != -1) {
                close(error_fd);
            }

            // Liberamos la memoria de la matriz dinámica de pipes
            for (int i = 0; i < num_comandos - 1; i++) {
                free(pipefd[i]); // Libera cada pipe
            }
            free(pipefd); // Libera el arreglo principal

            // Esperamos a los procesos hijos si se ha ejecutado en fg
            if (linea->background == 0) {
                for (int i = 0; i < num_comandos; i++) {
                    wait(NULL);
                }
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
        for (int i = 0; i < contador_jobs; i++) {
            if (jobs[i].pid == pid) {
                jobs[i].activo = 0; // El proceso ya no está activo
                if (WIFEXITED(status)) { // !=0 si el hijo ha terminado
                    strncpy(jobs[i].estado, "Done", sizeof(jobs[i].estado));
                }
                break;
            }
        }
    }
}

void fg(char* indice) {

    int id_job = -1;
    int indice_job = -1;
    int existe = 0;

    // Si se proporciona un ID de job, buscamos el job correspondiente
    if (indice != NULL) {
        id_job = atoi(indice);
        for (int i = 0; i < contador_jobs; i++) {
            if (jobs[i].id == id_job && jobs[i].activo) {
                indice_job = i;
                existe = 1;
                break;
            }
        }
    } else {
        // Si no se proporciona ID, usamos el último job activo
        for (int i = 0; i < contador_jobs; i++) {
            if (jobs[i].activo) {
                indice_job = i;
                id_job = jobs[i].id;
                existe = 1;
                break;
            }
        }
    }

    if (!existe || indice_job == -1) {
        fprintf(stderr, "fg: No existe un job activo con ese ID\n");
        return;
    }

    job_t *job = &jobs[indice_job];
    printf("Reanudando proceso [%d] %s\n", job->id, job->comando);

    waitpid(jobs[id_job].pid, NULL, 0);

    jobs[id_job].activo = 0;
    strncpy(job->estado, "Done", sizeof(job->estado)); 
}
