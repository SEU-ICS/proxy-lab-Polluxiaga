#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define MAX_CACHE_ENTRIES 10

/* You won't lose style points for including this long line in your code */
static const char *DEFAULT_USER_AGENT = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

// 读写锁结构
typedef struct {
    sem_t mutex;
    sem_t write_lock;
    int reader_count;
} rw_lock_t;

// Cache结构
typedef struct {
    int recently_used;
    char url[MAXLINE];
    char data[MAX_OBJECT_SIZE];
} CacheEntry;

// URL结构
typedef struct {
    char hostname[MAXLINE];
    char port[MAXLINE];
    char path[MAXLINE];
} ParsedUrl;

CacheEntry cache[MAX_CACHE_ENTRIES];
rw_lock_t* rw_lock;
int lru_index;

void process_request(int fd);
void parse_url(char* url, ParsedUrl* parsed_url);
void format_http_request(rio_t* rio, ParsedUrl* parsed_url, char* formatted_request);
void *handle_thread(void* connfd_ptr);
void init_rw_lock();
int find_cache_entry(int fd, char* url);
void add_cache_entry(char* data, char* url);

int main(int argc, char** argv) {
    rw_lock = Malloc(sizeof(rw_lock_t));
    pthread_t thread_id;
    int listen_fd, connection_fd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t client_len;
    struct sockaddr_storage client_addr;

    init_rw_lock();

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(0);
    }

    listen_fd = Open_listenfd(argv[1]);

    while (1) {
        client_len = sizeof(client_addr);
        connection_fd = Accept(listen_fd, (SA*)&client_addr, &client_len);
        Getnameinfo((SA*)&client_addr, client_len, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s %s)\n", hostname, port);
        Pthread_create(&thread_id, NULL, handle_thread, (void*)&connection_fd);
    }

    return 0;
}

void init_rw_lock() {
    rw_lock->reader_count = 0;
    sem_init(&rw_lock->mutex, 0, 1);
    sem_init(&rw_lock->write_lock, 0, 1);
}

void *handle_thread(void* connfd_ptr) {
    int conn_fd = *(int*)connfd_ptr;
    Pthread_detach(pthread_self());
    process_request(conn_fd);
    close(conn_fd);
    return NULL;
}

void process_request(int fd) {
    char buffer[MAXLINE], method[MAXLINE], url[MAXLINE], version[MAXLINE];
    char formatted_request[MAXLINE], cache_key[MAXLINE];
    ParsedUrl parsed_url;
    rio_t rio, server_rio;
    int server_fd;
    size_t n;

    Rio_readinitb(&rio, fd);
    Rio_readlineb(&rio, buffer, MAXLINE);

    sscanf(buffer, "%s %s %s", method, url, version);
    strcpy(cache_key, url);

    if (strcasecmp(method, "GET") != 0) {
        printf("Unsupported method: %s\n", method);
        return;
    }

    if (find_cache_entry(fd, cache_key)) {
        return;
    }

    parse_url(url, &parsed_url);
    format_http_request(&rio, &parsed_url, formatted_request);

    server_fd = Open_clientfd(parsed_url.hostname, parsed_url.port);
    Rio_readinitb(&server_rio, server_fd);
    Rio_writen(server_fd, formatted_request, strlen(formatted_request));

    char response_cache[MAX_OBJECT_SIZE];
    int total_size = 0;

    while ((n = Rio_readlineb(&server_rio, buffer, MAXLINE)) != 0) {
        Rio_writen(fd, buffer, n);
        total_size += n;
        strcat(response_cache, buffer);
    }

    printf("Proxy sent %d bytes to client\n", total_size);

    if (total_size < MAX_OBJECT_SIZE) {
        add_cache_entry(response_cache, cache_key);
    }

    close(server_fd);
}

void add_cache_entry(char* data, char* url) {
    sem_wait(&rw_lock->write_lock);
    int index;

    while (cache[lru_index].recently_used != 0) {
        cache[lru_index].recently_used = 0;
        lru_index = (lru_index + 1) % MAX_CACHE_ENTRIES;
    }

    index = lru_index;

    cache[index].recently_used = 1;
    strcpy(cache[index].url, url);
    strcpy(cache[index].data, data);

    sem_post(&rw_lock->write_lock);
}

int find_cache_entry(int fd, char* url) {
    sem_wait(&rw_lock->mutex);

    if (rw_lock->reader_count == 0) {
        sem_wait(&rw_lock->write_lock);
    }
    rw_lock->reader_count++;
    sem_post(&rw_lock->mutex);

    int i, found = 0;
    for (i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (strcmp(url, cache[i].url) == 0) {
            Rio_writen(fd, cache[i].data, strlen(cache[i].data));
            printf("Cache hit: sent %d bytes to client\n", strlen(cache[i].data));
            cache[i].recently_used = 1;
            found = 1;
            break;
        }
    }

    sem_wait(&rw_lock->mutex);
    rw_lock->reader_count--;

    if (rw_lock->reader_count == 0) {
        sem_post(&rw_lock->write_lock);
    }
    sem_post(&rw_lock->mutex);

    return found;
}

void parse_url(char* url, ParsedUrl* parsed_url) {
    char* host_position = strstr(url, "//");
    if (host_position == NULL) {
        char* path_position = strstr(url, "/");
        if (path_position != NULL) {
            strcpy(parsed_url->path, path_position);
        }
        strcpy(parsed_url->port, "80");
        return;
    } else {
        char* port_position = strstr(host_position + 2, ":");
        if (port_position != NULL) {
            int port_number;
            sscanf(port_position + 1, "%d%s", &port_number, parsed_url->path);
            sprintf(parsed_url->port, "%d", port_number);
            *port_position = '\0';
        } else {
            char* path_position = strstr(host_position + 2, "/");
            if (path_position != NULL) {
                strcpy(parsed_url->path, path_position);
                strcpy(parsed_url->port, "80");
                *path_position = '\0';
            }
        }
        strcpy(parsed_url->hostname, host_position + 2);
    }
}

void format_http_request(rio_t* rio, ParsedUrl* parsed_url, char* formatted_request) {
    static const char* CONNECTION_CLOSE = "Connection: close\r\n";
    static const char* PROXY_CONNECTION_CLOSE = "Proxy-Connection: close\r\n";
    char buffer[MAXLINE], request_line[MAXLINE], host_header[MAXLINE], other_headers[MAXLINE];

    sprintf(request_line, "GET %s HTTP/1.0\r\n", parsed_url->path);

    while (Rio_readlineb(rio, buffer, MAXLINE) > 0) {
        if (strcmp(buffer, "\r\n") == 0) {
            strcat(other_headers, "\r\n");
            break;
        } else if (strncasecmp(buffer, "Host:", 5) == 0) {
            strcpy(host_header, buffer);
        } else if (!strncasecmp(buffer, "Connection:", 11) && !strncasecmp(buffer, "Proxy-Connection:", 17) && !strncasecmp(buffer, "User-agent:", 11)) {
            strcat(other_headers, buffer);
        }
    }

    if (!strlen(host_header)) {
        sprintf(host_header, "Host: %s\r\n", parsed_url->hostname);
    }

    sprintf(formatted_request, "%s%s%s%s%s%s", request_line, host_header, CONNECTION_CLOSE, PROXY_CONNECTION_CLOSE, DEFAULT_USER_AGENT, other_headers);
}
