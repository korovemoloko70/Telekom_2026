#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>

#define PORT        8080
#define BUFFER_SIZE 4096

// читает до \n, возвращает длину без \n или -1
static int recv_line(int fd, char *buf, size_t bufsize) {
    size_t i = 0;
    while (i + 1 < bufsize) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == '\n') {
            buf[i] = '\0';
            return (int)i;
        }
        buf[i++] = c;
    }
    buf[i] = '\0';
    return (int)i;
}

static int recv_exact(int fd, char *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t k = recv(fd, buf + got, n - got, 0);
        if (k <= 0) return -1;
        got += (size_t)k;
    }
    return 0;
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

// читает из сокета ровно size байт и кладёт в downloaded_<name>
static int save_file_from_socket(int sock, const char *name, size_t size) {
    char out_name[512];
    snprintf(out_name, sizeof(out_name), "downloaded_%s", name);

    int out_fd = open(out_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        perror("open output");
        return -1;
    }

    char buf[BUFFER_SIZE];
    size_t remaining = size;
    while (remaining > 0) {
        size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
        if (recv_exact(sock, buf, chunk) < 0) {
            fprintf(stderr, "Connection closed while receiving %s\n", name);
            close(out_fd);
            unlink(out_name);
            return -1;
        }
        size_t written = 0;
        while (written < chunk) {
            ssize_t k = write(out_fd, buf + written, chunk - written);
            if (k <= 0) {
                perror("write");
                close(out_fd);
                unlink(out_name);
                return -1;
            }
            written += (size_t)k;
        }
        remaining -= chunk;
    }
    close(out_fd);
    printf("  -> %s (%zu bytes)\n", out_name, size);
    return 0;
}

static int request_file(int sock, const char *filename) {
    char cmd[BUFFER_SIZE];
    int clen = snprintf(cmd, sizeof(cmd), "GET %s\n", filename);
    if (send_all(sock, cmd, (size_t)clen) < 0) {
        perror("send");
        return -1;
    }

    char line[BUFFER_SIZE];
    if (recv_line(sock, line, sizeof(line)) < 0) {
        fprintf(stderr, "Connection closed by server\n");
        return -1;
    }

    if (strncmp(line, "OK ", 3) == 0) {
        size_t size = (size_t)atol(line + 3);
        printf("Receiving %s (%zu bytes)...\n", filename, size);
        return save_file_from_socket(sock, filename, size);
    } else {
        printf("Error for %s: %s\n", filename, line);
        return -1;
    }
}

static int request_multi(int sock, char **filenames, int nfiles) {
    char cmd[BUFFER_SIZE];
    int off = snprintf(cmd, sizeof(cmd), "GETM");
    for (int i = 0; i < nfiles; i++) {
        off += snprintf(cmd + off, sizeof(cmd) - (size_t)off, " %s", filenames[i]);
        if (off >= (int)sizeof(cmd) - 2) {
            fprintf(stderr, "Command too long\n");
            return -1;
        }
    }
    cmd[off++] = '\n';
    cmd[off] = '\0';

    if (send_all(sock, cmd, (size_t)off) < 0) {
        perror("send");
        return -1;
    }

    char line[BUFFER_SIZE];
    if (recv_line(sock, line, sizeof(line)) < 0) {
        fprintf(stderr, "Connection closed by server\n");
        return -1;
    }
    if (strncmp(line, "MULTI ", 6) != 0) {
        fprintf(stderr, "Unexpected response: %s\n", line);
        return -1;
    }
    int n = atoi(line + 6);
    printf("Server will send %d file(s)\n", n);

    for (int i = 0; i < n; i++) {
        if (recv_line(sock, line, sizeof(line)) < 0) {
            fprintf(stderr, "Connection closed mid-transfer\n");
            return -1;
        }
        // FILE <имя> OK <размер>   или   FILE <имя> ERR <код>
        char name[256], status[16];
        long size = -1;
        int parsed = sscanf(line, "FILE %255s %15s %ld", name, status, &size);
        if (parsed < 3) {
            fprintf(stderr, "Malformed header: %s\n", line);
            return -1;
        }
        if (strcmp(status, "OK") == 0) {
            printf("Receiving %s (%ld bytes)...\n", name, size);
            if (save_file_from_socket(sock, name, (size_t)size) < 0) return -1;
        } else {
            fprintf(stderr, "Error for %s: %s\n", name, status);
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
            "Usage: %s <host> <file1> [file2 ...] [--multi]\n"
            "Example: %s 127.0.0.1 test1.txt test2.txt\n"
            "         %s 127.0.0.1 test1.txt test2.txt --multi\n",
            argv[0], argv[0], argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int multi = 0;

    char **files = calloc((size_t)argc, sizeof(char*));
    int nfiles = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--multi") == 0) multi = 1;
        else files[nfiles++] = argv[i];
    }
    if (nfiles == 0) {
        fprintf(stderr, "No files specified\n");
        free(files);
        return 1;
    }

    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", PORT);

    int rv = getaddrinfo(host, port_str, &hints, &res);
    if (rv != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        free(files);
        return 1;
    }

    int sock = -1;
    for (p = res; p != NULL; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, p->ai_addr, p->ai_addrlen) == 0) break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);

    if (sock < 0) {
        fprintf(stderr, "Failed to connect to %s:%d\n", host, PORT);
        free(files);
        return 1;
    }

    int ret = 0;
    if (multi) {
        ret = request_multi(sock, files, nfiles);
    } else {
        for (int i = 0; i < nfiles; i++) {
            if (request_file(sock, files[i]) < 0) { ret = -1; break; }
        }
    }

    close(sock);
    free(files);
    return ret == 0 ? 0 : 1;
}