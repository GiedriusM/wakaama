/*
 * MIT License
 *
 * Copyright (c) 2017 8devices
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define _GNU_SOURCE
#include <pthread.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include "rest-ssdp.h"
#include "restserver.h"



#define HELLO_PORT "1900"
#define HELLO_GROUP "ff02::c"
#define MSGBUFSIZE 1025

//extern volatile int get_restserver_quit(void);
static void* Udp6Listener(void *arg);
#define QUIT_SIGNAL_NO SIGUSR1
static volatile int ssdp_quit=0;
static volatile pthread_t thread_h;
static volatile pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

const char *response="HTTP/1.1 200 OK\r\n"
                     "CACHE-CONTROL: max-age=60\r\n"
                     "EXT:\r\n"
                     "LOCATION: http://[%s]: %s /descriptiondocname\r\n"
                     "SERVER: OS/0.1 UPnP/1.0 X/0.1\r\n"
                     "ST: urn:8devices-com:service:lwm2m:1\r\n"
                     "USN: uuid:d03a8702-4d3f-46e8-a33d-9affcc754e2f::urn:8devices-com:service:lwm2m:1\r\n"
                     "\r\n";



void start_ssdp(void)
{
    pthread_mutex_lock((pthread_mutex_t *)&mutex);
    if(thread_h != 0)
        stop_ssdp();


    int err = pthread_create((pthread_t*)&thread_h, NULL, &Udp6Listener, NULL);
    if (err != 0) {
        fprintf(stderr, "\ncan't create thread :[%s]", strerror(err));
        pthread_mutex_unlock((pthread_mutex_t *)&mutex);
        exit(EXIT_FAILURE);
    }
    else{
        pthread_setname_np(thread_h, "ssdp_server");
        printf("\n Thread created successfully\n");
    }
    pthread_mutex_unlock((pthread_mutex_t *)&mutex);
}

void stop_ssdp(void)
{
    pthread_mutex_lock((pthread_mutex_t *)&mutex);

    pthread_kill(thread_h,QUIT_SIGNAL_NO);
    //ssdp_quit=1;

    if(thread_h != 0)
    {
        pthread_join(thread_h,NULL);
        thread_h = 0;
    }
    if(ssdp_quit)
        ssdp_quit=0;
    else
        fprintf(stderr, "\nCan't stop ssdp thread");
    pthread_mutex_unlock((pthread_mutex_t *)&mutex);
}

static int parse_buf_lines(char* buf)
{
    char *lines[10];
    const int  lines_size = (sizeof(lines) / sizeof(lines[0])) ;
    int i = 0;
    memset(lines, 0, sizeof(lines));
    char* buf_save=buf;

    lines[i] = strtok_r(buf, "\r\n",&buf_save);

    while (lines[i] != NULL && i < lines_size  ) {
        lines[++i] = strtok_r(NULL, "\r\n",&buf_save);
    }

    if (strcasecmp("M-SEARCH * HTTP/1.1", lines[0]) != 0) {
        fprintf(stderr, "Invalid request header: %s", lines[0]);
        return 0;
    }

    i = 1;
    while (lines[i] != NULL && i < lines_size) {
        if (strcmp("ST: urn:8devices-com:service:lwm2m:1", lines[i]) == 0)
            return 1;
        i++;
    }

    return 0;
}

static void ssdp_quit_sig_handler(int signo)
{
    ssdp_quit=1;
}


static void* Udp6Listener(void *arg)
{
    char msgbuf[MSGBUFSIZE];
    socklen_t addrlen;
    struct sockaddr_storage clientaddr;
    char clienthost[NI_MAXHOST];
    char clientservice[NI_MAXSERV];

    char* multicastIP = HELLO_GROUP;
    char* multicastPort = HELLO_PORT;

    int sock;
    struct addrinfo hints = { 0 }; /* Hints for name lookup */
    struct addrinfo* localAddr = 0; /* Local address to bind to */
    struct addrinfo* multicastAddr = 0; /* Multicast Address */
    int yes = 1;
/////////////////////////////////////
    ssdp_quit=0;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = &ssdp_quit_sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (0 != sigaction(QUIT_SIGNAL_NO, &sa, NULL) ) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
/////////////////////////////////////


    /* Resolve the multicast group address */
    hints.ai_family = PF_UNSPEC;
    hints.ai_flags = AI_NUMERICHOST;
    int status;
    if ((status = getaddrinfo(multicastIP, NULL, &hints, &multicastAddr)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        goto error;
    }

    /*
     Get a local address with the same family (IPv4 or IPv6) as our multicast group
     This is for receiving on a certain port.
     */
    hints.ai_family = multicastAddr->ai_family;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE; /* Return an address we can bind to */
    if (getaddrinfo(NULL, multicastPort, &hints, &localAddr) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        goto error;
    }

    /* Create socket for receiving datagrams */
    if ((sock = socket(localAddr->ai_family, localAddr->ai_socktype, 0)) < 0) {
        perror("socket() failed");
        goto error;
    }

    /*
     * Enable SO_REUSEADDR to allow multiple instances of this
     * application to receive copies of the multicast datagrams.
     */
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*) &yes, sizeof(int)) == -1) {
        perror("setsockopt");
        goto error;
    }

    /* Bind the local address to the multicast port */
    if (bind(sock, localAddr->ai_addr, localAddr->ai_addrlen) != 0) {
        perror("bind() failed");
        goto error;
    }

    /* Join the multicast group. We do this seperately depending on whether we
     * are using IPv4 or IPv6.
     */
    if (multicastAddr->ai_family == PF_INET && multicastAddr->ai_addrlen == sizeof(struct sockaddr_in)) /* IPv4 */
    {
        struct ip_mreq multicastRequest; /* Multicast address join structure */

        /* Specify the multicast group */
        memcpy(&multicastRequest.imr_multiaddr, &((struct sockaddr_in*) (multicastAddr->ai_addr))->sin_addr, sizeof(multicastRequest.imr_multiaddr));

        /* Accept multicast from any interface */
        multicastRequest.imr_interface.s_addr = htonl(INADDR_ANY);

        /* Join the multicast address */
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*) &multicastRequest, sizeof(multicastRequest)) != 0) {
            perror("setsockopt() failed");
            goto error;
        }
    }
    else if (multicastAddr->ai_family == PF_INET6 && multicastAddr->ai_addrlen == sizeof(struct sockaddr_in6)) /* IPv6 */
    {
        struct ipv6_mreq multicastRequest; /* Multicast address join structure */

        /* Specify the multicast group */
        memcpy(&multicastRequest.ipv6mr_multiaddr, &((struct sockaddr_in6*) (multicastAddr->ai_addr))->sin6_addr, sizeof(multicastRequest.ipv6mr_multiaddr));

        /* Accept multicast from any interface */
        multicastRequest.ipv6mr_interface = 0;

        /* Join the multicast address */
        if (setsockopt(sock, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, (char*) &multicastRequest, sizeof(multicastRequest)) != 0) {
            perror("setsockopt() failed");
            goto error;
        }
    }
    else {
        perror("Neither IPv4 or IPv6");
        goto error;
    }

    printf("Listening on all addresses\n");

    /* now just enter a read-print loop */
    while (!ssdp_quit)
    {
        addrlen = sizeof(clientaddr);
        int nbytes = recvfrom(sock, msgbuf, MSGBUFSIZE - 1, 0, (struct sockaddr *) &clientaddr, &addrlen);

        if (nbytes < 0) {
            perror("recvfrom");
            continue;
        }

        if (nbytes < (MSGBUFSIZE - 1) )
            msgbuf[nbytes] = '\0';
        else
            msgbuf[MSGBUFSIZE - 1] = '\0';

        memset(clienthost, 0, sizeof(clienthost));
        memset(clientservice, 0, sizeof(clientservice));

        getnameinfo((struct sockaddr *) &clientaddr, addrlen, clienthost, sizeof(clienthost), clientservice, sizeof(clientservice),
        NI_NUMERICHOST | NI_NUMERICSERV | NI_NOFQDN);

        printf("Received request from host=[%s] port=[%s]\n", clienthost, clientservice);

        puts(msgbuf);

        if (parse_buf_lines(msgbuf)) {
            snprintf(msgbuf, sizeof(msgbuf), response, clienthost, UDP_LISTENER_PORT_NB);//currently return client host address IPV6 and COAP port number to get local host, maybe need use getifaddrs()
            puts(msgbuf);
            nbytes = sendto(sock, msgbuf, strlen(msgbuf), 0, (struct sockaddr *) &clientaddr, sizeof(clientaddr));
            if (nbytes < 1)
                perror("sendto error:: \n");
        }
    } //while

    close(sock);

    if (localAddr)
        freeaddrinfo(localAddr);
    if (multicastAddr)
        freeaddrinfo(multicastAddr);

    return (void*) 0;

    error: if (localAddr)
        freeaddrinfo(localAddr);
    if (multicastAddr)
        freeaddrinfo(multicastAddr);

    return (void*) -1;
}

