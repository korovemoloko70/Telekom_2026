#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>

#define PORT          8080
#define BUFFER_SIZE   4096
#define MAX_CLIENTS   64

static char *root_dir = "./server_files";

static void send_str(int fd, const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    send(fd, buf, strlen(buf), 0);
}

static int send_all(int fd, const char *buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t k = send(fd, buf + sent, n - sent, 0);
        if (k <= 0) return -1;
        sent += (size_t)k;
    }
    return 0;
}

static int handle_get(int client_fd, const char *filename) {
    char filepath[BUFFER_SIZE];

    // не даём вылезти за пределы root_dir
    if (strstr(filename, "..") != NULL) {
        send_str(client_fd, "ERR 403 Forbidden\n");
        return -1;
    }

    snprintf(filepath, sizeof(filepath), "%s/%s", root_dir, filename);

    int file_fd = open(filepath, O_RDONLY);
    if (file_fd < 0) {
        send_str(client_fd, "ERR 404 Not Found\n");
        return -1;
    }

    struct stat st;
    fstat(file_fd, &st);
    send_str(client_fd, "OK %ld\n", st.st_size);

    char file_buf[BUFFER_SIZE];
    ssize_t bytes_read;
    while ((bytes_read = read(file_fd, file_buf, sizeof(file_buf))) > 0) {
        if (send_all(client_fd, file_buf, (size_t)bytes_read) < 0) {
            close(file_fd);
            return -1;
        }
    }
    close(file_fd);
    return 0;
}

static int handle_getm(int client_fd, const char *args) {
    char args_copy[BUFFER_SIZE];
    snprintf(args_copy, sizeof(args_copy), "%s", args);

    char *filenames[64];
    int nfiles = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(args_copy, " \t", &saveptr);
    while (tok != NULL && nfiles < 64) {
        filenames[nfiles++] = tok;
        tok = strtok_r(NULL, " \t", &saveptr);
    }

    if (nfiles == 0) {
        send_str(client_fd, "ERR 400 Bad Request\n");
        return -1;
    }

    send_str(client_fd, "MULTI %d\n", nfiles);

    for (int i = 0; i < nfiles; i++) {
        const char *name = filenames[i];

        if (strstr(name, "..") != NULL) {
            send_str(client_fd, "FILE %s ERR 403\n", name);
            continue;
        }

        char filepath[BUFFER_SIZE];
        snprintf(filepath, sizeof(filepath), "%s/%s", root_dir, name);

        int file_fd = open(filepath, O_RDONLY);
        if (file_fd < 0) {
            send_str(client_fd, "FILE %s ERR 404\n", name);
            continue;
        }

        struct stat st;
        fstat(file_fd, &st);
        send_str(client_fd, "FILE %s OK %ld\n", name, st.st_size);

        char file_buf[BUFFER_SIZE];
        ssize_t bytes_read;
        int failed = 0;
        while ((bytes_read = read(file_fd, file_buf, sizeof(file_buf))) > 0) {
            if (send_all(client_fd, file_buf, (size_t)bytes_read) < 0) {
                failed = 1;
                break;
            }
        }
        close(file_fd);
        if (failed) return -1;
    }
    return 0;
}

int main(void) {
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", PORT);

    int rv = getaddrinfo(NULL, port_str, &hints, &res);
    if (rv != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        exit(1);
    }

    int listen_fd = -1;
    for (p = res; p != NULL; p = p->ai_next) {
        listen_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (listen_fd < 0) continue;

        int yes = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        if (bind(listen_fd, p->ai_addr, p->ai_addrlen) == 0) break;

        close(listen_fd);
        listen_fd = -1;
    }
    freeaddrinfo(res);

    if (listen_fd < 0) {
        perror("failed to bind");
        exit(1);
    }
    if (listen(listen_fd, 10) < 0) {
        perror("listen");
        exit(1);
    }

    printf("Server listening on port %d, serving from: %s\n", PORT, root_dir);
    mkdir(root_dir, 0755);

    // чтобы не копились зомби от fork()
    signal(SIGCHLD, SIG_IGN);

    fd_set master, read_fds;
    FD_ZERO(&master);
    FD_ZERO(&read_fds);
    FD_SET(listen_fd, &master);
    int fdmax = listen_fd;

    char client_bufs[MAX_CLIENTS][BUFFER_SIZE];
    int  client_buf_len[MAX_CLIENTS];
    memset(client_buf_len, 0, sizeof(client_buf_len));

    while (1) {
        read_fds = master;
        if (select(fdmax + 1, &read_fds, NULL, NULL, NULL) == -1) {
            if (errno == EINTR) continue;
            perror("select");
            exit(1);
        }

        for (int fd = 0; fd <= fdmax; fd++) {
            if (!FD_ISSET(fd, &read_fds)) continue;

            if (fd == listen_fd) {
                struct sockaddr_storage client_addr;
                socklen_t addrlen = sizeof(client_addr);
                int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &addrlen);
                if (client_fd < 0) {
                    perror("accept");
                    continue;
                }
                if (client_fd >= MAX_CLIENTS) {
                    fprintf(stderr, "Too many clients, rejecting fd=%d\n", client_fd);
                    close(client_fd);
                    continue;
                }

                char ip[INET6_ADDRSTRLEN];
                if (client_addr.ss_family == AF_INET) {
                    struct sockaddr_in *s = (struct sockaddr_in*)&client_addr;
                    inet_ntop(AF_INET, &s->sin_addr, ip, sizeof(ip));
                } else {
                    struct sockaddr_in6 *s = (struct sockaddr_in6*)&client_addr;
                    inet_ntop(AF_INET6, &s->sin6_addr, ip, sizeof(ip));
                }
                printf("New connection from %s (fd=%d)\n", ip, client_fd);

                FD_SET(client_fd, &master);
                if (client_fd > fdmax) fdmax = client_fd;
                client_buf_len[client_fd] = 0;
            } else {
                char temp[BUFFER_SIZE];
                int n = recv(fd, temp, sizeof(temp), 0);

                if (n <= 0) {
                    if (n == 0) printf("Client fd=%d disconnected\n", fd);
                    else perror("recv");
                    close(fd);
                    FD_CLR(fd, &master);
                    client_buf_len[fd] = 0;
                    if (fd == fdmax) {
                        while (fdmax > 0 && !FD_ISSET(fdmax, &master)) fdmax--;
                    }
                    continue;
                }

                // TCP — поток байт, поэтому копим и режем по \n
                int off = client_buf_len[fd];
                if (off + n > BUFFER_SIZE) {
                    fprintf(stderr, "Buffer overflow on fd=%d\n", fd);
                    close(fd);
                    FD_CLR(fd, &master);
                    client_buf_len[fd] = 0;
                    continue;
                }
                memcpy(client_bufs[fd] + off, temp, n);
                off += n;

                int start = 0;
                for (int i = 0; i < off; i++) {
                    if (client_bufs[fd][i] == '\n') {
                        client_bufs[fd][i] = '\0';
                        char *line = client_bufs[fd] + start;

                        // GETM проверяем раньше GET, иначе GETM распарсится как GET файла "M ..."
                        if (strncmp(line, "GETM ", 5) == 0) {
                            handle_getm(fd, line + 5);
                        } else if (strncmp(line, "GET ", 4) == 0) {
                            handle_get(fd, line + 4);
                        } else if (line[0] != '\0') {
                            send_str(fd, "ERR 400 Bad Request\n");
                        }
                        start = i + 1;
                    }
                }
                int remaining = off - start;
                if (remaining > 0 && start > 0) {
                    memmove(client_bufs[fd], client_bufs[fd] + start, remaining);
                }
                client_buf_len[fd] = remaining;
            }
        }
    }
    return 0;
}