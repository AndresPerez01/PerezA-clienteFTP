/*
 * COMPILAR: gcc -o PerezA-clienteFTP PerezA-clienteFTP.c connectsock.c connectTCP.c errexit.c -lpthread
 * USO: ./PerezA-clienteFTP localhost 21 <servidor> [puerto]
 * EJEMPLO: ./PerezA-clienteFTP localhost 21 localhost 21
 * PD: En permisos de escritura en cualquier servidor si se generan errores, usar chmod 755 y 775 en el servidor, por si las dudas.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>

#define BUFSIZE 4096
#define MAX_TRANSFERS 10
#define FTP_PORT "21"

extern int connectTCP(const char *host, const char *service);
extern int errexit(const char *format, ...);

// Enumeraciones para modos de transferencia
typedef enum { MODE_PASSIVE, MODE_ACTIVE } transfer_mode_t;
typedef enum { TRANSFER_UPLOAD, TRANSFER_DOWNLOAD } transfer_type_t;

// Estructura para gestionar transferencias concurrentes
typedef struct {
    int id, active;
    transfer_type_t type;
    char local_file[256], remote_file[256];
    pid_t pid;
} transfer_info_t;

static int control_sock = -1;
static transfer_mode_t transfer_mode = MODE_PASSIVE;
static transfer_info_t transfers[MAX_TRANSFERS];
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static int next_id = 1;

// Lee respuesta multilínea del servidor FTP
int ftp_read_response(int sock, char *buf, int size) {
    char line[BUFSIZE], c;
    int n, code = 0, first = 1, remaining = size - 1;
    char *ptr = buf;
    buf[0] = '\0';
    
    while (1) {
        for (n = 0; n < BUFSIZE - 1 && read(sock, &c, 1) > 0 && (line[n++] = c) != '\n'; );
        line[n] = '\0';
        
        if ((n = strlen(line)) < remaining) {
            strcat(ptr, line);
            ptr += n;
            remaining -= n;
        }
        
        if (first && sscanf(line, "%d", &code) == 1) first = 0;
        if (line[3] == ' ') break;
    }
    return code;
}

// Envía comando y lee respuesta
int ftp_command(int sock, const char *cmd, char *resp, int size) {
    char buf[BUFSIZE];
    printf(">>> %s\n", cmd);
    snprintf(buf, BUFSIZE, "%s\r\n", cmd);
    if (write(sock, buf, strlen(buf)) < 0) return -1;
    int code = ftp_read_response(sock, resp, size);
    printf("<<< %s", resp);
    return code;
}

// Wrapper para comandos simples con formato
int ftp_simple_cmd(const char *fmt, ...) {
    char cmd[256], resp[BUFSIZE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, args);
    va_end(args);
    return ftp_command(control_sock, cmd, resp, BUFSIZE);
}

// Modo pasivo: cliente inicia conexión de datos
int ftp_pasv(void) {
    char resp[BUFSIZE];
    if (ftp_command(control_sock, "PASV", resp, BUFSIZE) != 227) 
        return errexit("[ERROR] PASV falló\n");
    
    char *s = strchr(resp, '(');
    int h1, h2, h3, h4, p1, p2;
    if (!s || sscanf(s, "(%d,%d,%d,%d,%d,%d)", &h1, &h2, &h3, &h4, &p1, &p2) != 6) 
        return errexit("[ERROR] No se pudo parsear respuesta PASV\n");
    
    char ip[64], port[16];
    snprintf(ip, sizeof(ip), "%d.%d.%d.%d", h1, h2, h3, h4);
    snprintf(port, sizeof(port), "%d", p1 * 256 + p2);
    printf("[PASV] %s:%s\n", ip, port);
    
    return connectTCP(ip, port);
}

// Modo activo: servidor inicia conexión de datos
int ftp_port(void) {
    struct sockaddr_in addr, ctrl_addr;
    socklen_t len = sizeof(addr);
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return errexit("[ERROR] No se pudo crear socket\n");
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;
    
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(sock, 1) < 0 ||
        getsockname(sock, (struct sockaddr *)&addr, &len) < 0 ||
        getsockname(control_sock, (struct sockaddr *)&ctrl_addr, &len) < 0) {
        close(sock);
        return errexit("[ERROR] PORT setup falló\n");
    }
    
    unsigned char *ip = (unsigned char *)&ctrl_addr.sin_addr.s_addr;
    int port = ntohs(addr.sin_port);
    
    if (ftp_simple_cmd("PORT %d,%d,%d,%d,%d,%d", ip[0], ip[1], ip[2], ip[3], port/256, port%256) != 200) {
        close(sock);
        return errexit("[ERROR] Comando PORT falló\n");
    }
    
    printf("[PORT] Puerto %d\n", port);
    return sock;
}


// Función para transferencias de datos
int data_transfer(const char *cmd, int (*io_func)(int, void*, size_t), void *fp, int is_read) {
    char resp[BUFSIZE];
    int data_sock, code;
    
    if (transfer_mode == MODE_PASSIVE) {
        if ((data_sock = ftp_pasv()) < 0) return -1;
        if ((code = ftp_command(control_sock, cmd, resp, BUFSIZE)) != 150 && code != 125) {
            close(data_sock);
            return -1;
        }
    } else {
        int listen_sock = ftp_port();
        if (listen_sock < 0) return -1;
        
        snprintf(resp, BUFSIZE, "%s\r\n", cmd);
        write(control_sock, resp, strlen(resp));
        
        data_sock = accept(listen_sock, NULL, NULL);
        close(listen_sock);
        if (data_sock < 0) return -1;
        
        ftp_read_response(control_sock, resp, BUFSIZE);
        printf("<<< %s", resp);
    }
    
    char buf[BUFSIZE];
    int n, total = 0;
    
    if (is_read) {
        while ((n = read(data_sock, buf, BUFSIZE)) > 0) {
            fwrite(buf, 1, n, fp);
            total += n;
            if (total % (100*1024) == 0) printf("[%d KB]\n", total/1024);
        }
    } else {
        while ((n = fread(buf, 1, BUFSIZE, fp)) > 0) {
            write(data_sock, buf, n);
            total += n;
            if (total % (100*1024) == 0) printf("[%d KB]\n", total/1024);
        }
    }
    
    printf("[COMPLETO] %d bytes\n", total);
    close(data_sock);
    
    code = ftp_read_response(control_sock, resp, BUFSIZE);
    printf("<<< %s", resp);
    return (code == 226) ? 0 : -1;
}

// Descarga archivo del servidor
int ftp_retr(const char *remote, const char *local, long offset) {
    ftp_simple_cmd("TYPE I");
    if (offset > 0 && ftp_simple_cmd("REST %ld", offset) != 350) offset = 0;
    
    FILE *fp = fopen(local, offset > 0 ? "ab" : "wb");
    if (!fp) return errexit("[ERROR] No se pudo abrir archivo local: %s\n", local);
    
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "RETR %s", remote);
    printf("[DESCARGA] %s -> %s%s\n", remote, local, offset ? " (resumiendo)" : "");
    
    int ret = data_transfer(cmd, (int(*)(int,void*,size_t))read, fp, 1);
    fclose(fp);
    return ret;
}

// Sube archivo al servidor
int ftp_stor(const char *local, const char *remote) {
    ftp_simple_cmd("TYPE I");
    
    FILE *fp = fopen(local, "rb");
    if (!fp) return errexit("[ERROR] No se pudo abrir archivo local: %s\n", local);
    
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "STOR %s", remote);
    printf("[SUBIDA] %s -> %s\n", local, remote);
    
    int ret = data_transfer(cmd, (int(*)(int,void*,size_t))write, fp, 0);
    fclose(fp);
    return ret;
}

// Lista directorio remoto
int ftp_list(const char *path) {
    ftp_simple_cmd("TYPE A");
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "LIST%s%s", path ? " " : "", path ? path : "");
    
    char resp[BUFSIZE];
    int data_sock = (transfer_mode == MODE_PASSIVE) ? ftp_pasv() : ftp_port();
    if (data_sock < 0) return -1;
    
    if (transfer_mode == MODE_PASSIVE) {
        if (ftp_command(control_sock, cmd, resp, BUFSIZE) != 150 && 
            ftp_command(control_sock, cmd, resp, BUFSIZE) != 125) {
            close(data_sock);
            return -1;
        }
    } else {
        snprintf(resp, BUFSIZE, "%s\r\n", cmd);
        write(control_sock, resp, strlen(resp));
        int tmp = data_sock;
        data_sock = accept(tmp, NULL, NULL);
        close(tmp);
        ftp_read_response(control_sock, resp, BUFSIZE);
        printf("<<< %s", resp);
    }
    
    char buf[BUFSIZE];
    int n;
    printf("\n");
    while ((n = read(data_sock, buf, BUFSIZE - 1)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    printf("\n");
    close(data_sock);
    
    ftp_read_response(control_sock, resp, BUFSIZE);
    printf("<<< %s", resp);
    return 0;
}

//Metodos concurrentes:
// Proceso hijo que ejecuta la transferencia
void transfer_process(transfer_type_t type, char *local, char *remote, int id) {
    printf("[PROC %d] ID=%d iniciando\n", getpid(), id);
    int ret = (type == TRANSFER_DOWNLOAD) ? ftp_retr(remote, local, 0) : ftp_stor(local, remote);
    printf("[PROC %d] ID=%d %s\n", getpid(), id, ret ? "FALLIDA" : "EXITOSA");
    exit(ret ? 1 : 0);
}

// Inicia transferencia en segundo plano usando fork()
int start_transfer(transfer_type_t type, const char *local, const char *remote) {
    pthread_mutex_lock(&mutex);
    
    //Busca algun slot libre para realizar la transferencia
    int slot = -1;
    for (int i = 0; i < MAX_TRANSFERS && slot < 0; i++)
        if (!transfers[i].active) slot = i;
    
    if (slot < 0) {
        pthread_mutex_unlock(&mutex);
        printf("[ERROR] Límite alcanzado\n");
        return -1;
    }

    // Configura nueva transferencia
    int id = next_id++;
    transfers[slot] = (transfer_info_t){id, 1, type, "", "", 0};
    strncpy(transfers[slot].local_file, local, 255);
    strncpy(transfers[slot].remote_file, remote, 255);
    
    pthread_mutex_unlock(&mutex);
    
    // Crea proceso hijo para trabajar en el background
    pid_t pid = fork();
    if (pid < 0) {
        pthread_mutex_lock(&mutex);
        transfers[slot].active = 0;
        pthread_mutex_unlock(&mutex);
        return -1;
    }
    
    if (pid == 0) {
        char l[256], r[256];
        strncpy(l, local, 255);
        strncpy(r, remote, 255);
        transfer_process(type, l, r, id);
    }
    
    pthread_mutex_lock(&mutex);
    transfers[slot].pid = pid;
    pthread_mutex_unlock(&mutex);
    
    printf("[OK] ID=%d proceso=%d\n", id, pid);
    return id;
}

// Muestra transferencias activas
void list_transfers(void) {
    pthread_mutex_lock(&mutex);
    printf("\nID    TIPO       LOCAL                          REMOTO                         PID\n");
    printf("---   ----       -----                          ------                         ---\n");
    int cnt = 0;
    for (int i = 0; i < MAX_TRANSFERS; i++)
        if (transfers[i].active) {
            printf("%-5d %-10s %-30s %-30s %d\n", transfers[i].id,
                   transfers[i].type == TRANSFER_DOWNLOAD ? "DOWNLOAD" : "UPLOAD",
                   transfers[i].local_file, transfers[i].remote_file, transfers[i].pid);
            cnt++;
        }
    printf("\nTotal: %d\n\n", cnt);
    pthread_mutex_unlock(&mutex);
}

// Limpia procesos zombie terminados
void cleanup_transfers(void) {
    int status;
    pthread_mutex_lock(&mutex);
    for (int i = 0; i < MAX_TRANSFERS; i++)
        if (transfers[i].active && waitpid(transfers[i].pid, &status, WNOHANG) > 0) {
            printf("[FIN] ID=%d exit=%d\n", transfers[i].id, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            transfers[i].active = 0;
        }
    pthread_mutex_unlock(&mutex);
}

//Funcion main
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <servidor> [puerto]\n", argv[0]);
        exit(1);
    }
    
    char *server = argv[1], *port = argc >= 3 ? argv[2] : FTP_PORT;
    char user[256], pass[256], resp[BUFSIZE], cmd[BUFSIZE], arg1[256], arg2[256];
    
    memset(transfers, 0, sizeof(transfers));
    // Conexión inicial al servidor
    printf("[CONECTANDO] %s:%s\n", server, port);
    control_sock = connectTCP(server, port);
    
    if (ftp_read_response(control_sock, resp, BUFSIZE) != 220) 
        errexit("[ERROR] Respuesta inesperada del servidor\n");
    printf("<<< %s", resp);
    //Autenticacion con las credenciales del usuario
    printf("Usuario: ");
    fgets(user, sizeof(user), stdin);
    user[strcspn(user, "\n")] = 0;
    
    int code = ftp_simple_cmd("USER %s", user);
    if (code == 331) {
        printf("Password: ");
        fgets(pass, sizeof(pass), stdin);
        pass[strcspn(pass, "\n")] = 0;
        code = ftp_simple_cmd("PASS %s", pass);
    }
    
    if (code != 230) 
        errexit("[ERROR] Autenticación fallida\n");
    
    printf("[OK] Autenticado\n");
    printf("Comandos: get put ls/dir/list pwd cd mkdir delete jobs mode quit\n\n");
    // Bucle principal de comandos
    while (1) {
        cleanup_transfers();
        printf("ftp> ");
        fflush(stdout);
        
        if (!fgets(cmd, BUFSIZE, stdin)) break;
        cmd[strcspn(cmd, "\n")] = 0;
        if (!strlen(cmd)) continue;
        
        arg1[0] = arg2[0] = '\0';
        sscanf(cmd, "%*s %s %s", arg1, arg2);
        
        if (!strncmp(cmd, "get ", 4)) {
            if (!arg1[0]) { printf("Uso: get <remoto> [local]\n"); continue; }
            start_transfer(TRANSFER_DOWNLOAD, arg2[0] ? arg2 : arg1, arg1);
        } else if (!strncmp(cmd, "put ", 4)) {
            if (!arg1[0]) { printf("Uso: put <local> [remoto]\n"); continue; }
            start_transfer(TRANSFER_UPLOAD, arg1, arg2[0] ? arg2 : arg1);
        } else if (!strcmp(cmd, "jobs")) {
            list_transfers();
        } else if (!strcmp(cmd, "pwd")) {
            ftp_simple_cmd("PWD");
        } else if (!strncmp(cmd, "cd ", 3)) {
            if (arg1[0]) ftp_simple_cmd("CWD %s", arg1);
            else printf("Uso: cd <dir>\n");
        } else if (!strcmp(cmd, "ls") || !strcmp(cmd, "dir") || !strcmp(cmd, "list")) {
            ftp_list(arg1[0] ? arg1 : NULL);
        } else if (!strncmp(cmd, "mkdir ", 6)) {
            if (arg1[0]) ftp_simple_cmd("MKD %s", arg1);
            else printf("Uso: mkdir <dir>\n");
        } else if (!strncmp(cmd, "delete ", 7)) {
            if (arg1[0]) ftp_simple_cmd("DELE %s", arg1);
            else printf("Uso: delete <archivo>\n");
        } else if (!strncmp(cmd, "mode ", 5)) {
            if (!strcmp(arg1, "pasv")) {
                transfer_mode = MODE_PASSIVE;
                printf("[OK] PASV\n");
            } else if (!strcmp(arg1, "port")) {
                transfer_mode = MODE_ACTIVE;
                printf("[OK] PORT\n");
            } else {
                printf("Modo: %s\n", transfer_mode == MODE_PASSIVE ? "PASV" : "PORT");
            }
        } else if (!strcmp(cmd, "quit")) {
            printf("[CERRANDO]\n");
            // Espera a que terminen todas las transferencias
            int active;
            do {
                cleanup_transfers();
                pthread_mutex_lock(&mutex);
                active = 0;
                for (int i = 0; i < MAX_TRANSFERS; i++)
                    if (transfers[i].active) active++;
                pthread_mutex_unlock(&mutex);
                if (active > 0) {
                    printf("[ESPERANDO] %d activas\n", active);
                    sleep(1);
                }
            } while (active > 0);
            ftp_simple_cmd("QUIT");
            break;
        } else {
            printf("Comando desconocido: %s\n", cmd);
        }
    }
    
    close(control_sock);
    printf("[OK] Desconectado\n");
    return 0;

}
