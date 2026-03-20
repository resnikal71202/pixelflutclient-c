#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h> // for TCP_NODELAY
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <netdb.h>
#include <time.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

struct arg_struct
{
    int sockfd;
    char *buf;
    size_t buflen;
    struct addrinfo *server;
};

volatile uint8_t sig_exit = 0;
struct addrinfo *server;
struct addrinfo hints;

void siginthandler(int signum)
{
    sig_exit = 1;
}

static void set_sock_opts(int sockfd)
{
    int flag = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    int sndbuf = 1 << 20; // 1 MB send buffer
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
}

int reconnect(struct arg_struct *arg)
{
    close(arg->sockfd);
    int sockfd = socket(arg->server->ai_family, arg->server->ai_socktype, arg->server->ai_protocol);
    if (sockfd == -1)
    {
        perror("socket");
        return -1;
    }

    set_sock_opts(sockfd);

    if (connect(sockfd, arg->server->ai_addr, arg->server->ai_addrlen) == -1)
    {
        perror("connect");
        close(sockfd);
        return -1;
    }

    arg->sockfd = sockfd;
    return 0;
}

void *write_to_sock(void *args)
{
    struct arg_struct *arg = (struct arg_struct *)args;
    int failed = 0;
    while (!sig_exit && !failed)
    {
        size_t offset = 0;
        while (offset < arg->buflen && !sig_exit)
        {
            ssize_t written = send(arg->sockfd, arg->buf + offset, arg->buflen - offset, MSG_NOSIGNAL);
            if (written <= 0)
            {
                if (written == -1 && errno == EINTR)
                    continue;
                if (reconnect(arg) == -1)
                {
                    failed = 1;
                    break;
                }
                offset = 0;
            }
            else
            {
                offset += (size_t)written;
            }
        }
    }
    shutdown(arg->sockfd, SHUT_RDWR);
    close(arg->sockfd);
    free(arg);
    return NULL;
}

void shuffle(int *array, size_t n)
{
    if (n > 1)
    {
        for (size_t i = 0; i < n - 1; i++)
        {
            size_t j = i + rand() / (RAND_MAX / (n - i) + 1);
            int t = array[j];
            array[j] = array[i];
            array[i] = t;
        }
    }
}

int main(int argc, char *argv[])
{
    if (argc != 7)
    {
        fprintf(stderr, "Usage: %s <image_path> <x_offset> <y_offset> <hostname> <port> <num_threads>\n", argv[0]);
        return EXIT_FAILURE;
    }

    signal(SIGINT, siginthandler);

    int xoffset = atoi(argv[2]);
    int yoffset = atoi(argv[3]);

    int width, height, channels;
    unsigned char *img = stbi_load(argv[1], &width, &height, &channels, 0);
    if (!img)
    {
        fprintf(stderr, "Error loading image\n");
        return EXIT_FAILURE;
    }

    // Initialize random seed
    srand(time(NULL));

    int num_pixels = width * height;
    int *pixel_indices = malloc(num_pixels * sizeof(int));
    if (!pixel_indices)
    {
        fprintf(stderr, "Memory allocation error\n");
        stbi_image_free(img);
        return EXIT_FAILURE;
    }

    for (int i = 0; i < num_pixels; i++)
    {
        pixel_indices[i] = i;
    }

    shuffle(pixel_indices, num_pixels);

    size_t bufflen = 0;
    size_t buffcap = 1024;
    char *buff = malloc(buffcap);
    if (!buff)
    {
        fprintf(stderr, "Memory allocation error\n");
        free(pixel_indices);
        stbi_image_free(img);
        return EXIT_FAILURE;
    }

    for (int i = 0; i < num_pixels; i++)
    {
        int index = pixel_indices[i];
        int x = index % width;
        int y = index / width;
        int pixel_offset = index * channels;
        if (img[pixel_offset] != 0 ||
            img[pixel_offset + 1] != 0 ||
            img[pixel_offset + 2] != 0)
        {
            char temp[25];
            int diff = snprintf(temp, sizeof(temp), "PX %d %d %02x%02x%02x\n", x + xoffset, y + yoffset,
                     img[pixel_offset], img[pixel_offset + 1], img[pixel_offset + 2]);
            if (bufflen + (size_t)diff > buffcap)
            {
                buffcap *= 2;
                char *new_buff = realloc(buff, buffcap);
                if (!new_buff)
                {
                    free(buff);
                    free(pixel_indices);
                    stbi_image_free(img);
                    return EXIT_FAILURE;
                }
                buff = new_buff;
            }
            memcpy(&(buff[bufflen]), temp, (size_t)diff);
            bufflen += (size_t)diff;
        }
    }
    stbi_image_free(img);
    free(pixel_indices);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(argv[4], argv[5], &hints, &server) != 0)
    {
        fprintf(stderr, "getaddrinfo error\n");
        free(buff);
        return EXIT_FAILURE;
    }

    int num_threads = atoi(argv[6]);
    pthread_t threads[num_threads];

    for (int i = 0; i < num_threads; i++)
    {
        int sockfd = socket(server->ai_family, server->ai_socktype, server->ai_protocol);
        if (sockfd == -1)
        {
            perror("socket");
            free(buff);
            freeaddrinfo(server);
            return EXIT_FAILURE;
        }

        set_sock_opts(sockfd);

        struct arg_struct *arg = malloc(sizeof(struct arg_struct));
        if (!arg)
        {
            fprintf(stderr, "Memory allocation error\n");
            close(sockfd);
            free(buff);
            freeaddrinfo(server);
            return EXIT_FAILURE;
        }
        arg->sockfd = sockfd;
        arg->buf = buff;
        arg->buflen = bufflen;
        arg->server = server;

        if (connect(sockfd, server->ai_addr, server->ai_addrlen) == -1)
        {
            perror("connect");
            close(sockfd);
            free(arg);
            free(buff);
            freeaddrinfo(server);
            return EXIT_FAILURE;
        }

        if (pthread_create(&threads[i], NULL, write_to_sock, arg) != 0)
        {
            perror("pthread_create");
            close(sockfd);
            free(arg);
            free(buff);
            freeaddrinfo(server);
            return EXIT_FAILURE;
        }
    }

    for (int i = 0; i < num_threads; i++)
    {
        pthread_join(threads[i], NULL);
    }

    free(buff);
    freeaddrinfo(server);

    return EXIT_SUCCESS;
}