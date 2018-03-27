/*
 * rest-ssdp.c
 * Created on: Mar 23, 2018
 * Unpublished Copyright (c) 2017 8devices, All Rights Reserved.
 *
 * NOTICE: All information contained herein is, and remains the property of
 * 8devices. The intellectual and technical concepts contained herein are
 * proprietary to 8devices and are protected by trade secret or copyright law.
 * Dissemination of this information or reproduction of this material is
 * strictly forbidden unless prior written permission is obtained from 8devices.
 * Access to the source code contained herein is hereby forbidden to anyone
 * except current 8devices employees, managers or contractors who have executed
 * Confidentiality and Non-disclosure agreements explicitly covering such access.
 *
 * The copyright notice above does not evidence any actual or intended
 * publication or disclosure of this source code, which includes information
 * that is confidential and/or proprietary, and is a trade secret, of 8devices.
 * ANY REPRODUCTION, MODIFICATION, DISTRIBUTION, PUBLIC PERFORMANCE, OR PUBLIC
 * DISPLAY OF OR THROUGH USE OF THIS SOURCE CODE WITHOUT THE EXPRESS WRITTEN
 * CONSENT OF COMPANY IS STRICTLY PROHIBITED, AND IN VIOLATION OF APPLICABLE
 * LAWS AND INTERNATIONAL TREATIES. THE RECEIPT OR POSSESSION OF THIS SOURCE
 * CODE AND/OR RELATED INFORMATION DOES NOT CONVEY OR IMPLY ANY RIGHTS TO
 * REPRODUCE, DISCLOSE OR DISTRIBUTE ITS CONTENTS, OR TO MANUFACTURE, USE,
 * OR SELL ANYTHING THAT IT MAY DESCRIBE, IN WHOLE OR IN PART.
 */



#include "rest-ssdp.h"
#include "restserver.h"

//#define _GNU_SOURCE
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



#define HELLO_PORT "1900"
#define HELLO_GROUP "ff02::c"
#define MSGBUFSIZE 1025

extern volatile int get_restserver_quit(void);
static void* Udp6Listener(void *arg);

const char *response="HTTP/1.1 200 OK\r\n"
                     "CACHE-CONTROL: max-age=60\r\n"
                     "EXT:\r\n"
                     "LOCATION: http://[%s]: %s /descriptiondocname\r\n"
                     "SERVER: OS/0.1 UPnP/1.0 X/0.1\r\n"
                     "ST: urn:8devices-com:service:lwm2m:1\r\n"
                     "USN: uuid:d03a8702-4d3f-46e8-a33d-9affcc754e2f::urn:8devices-com:service:lwm2m:1\r\n"
                     "\r\n";


void init_ssdp(void)
{
    pthread_t thread_h;
    int err = pthread_create(&thread_h, NULL, &Udp6Listener, NULL);
    if (err != 0) {
        fprintf(stderr, "\ncan't create thread :[%s]", strerror(err));
        exit(EXIT_FAILURE);
    }
    else{
        pthread_setname_np(thread_h, "ssdp_server");
        printf("\n Thread created successfully\n");

    }

}

static int parse_buf_lines(char* buf)
{
    char *lines[10];
    const int  lines_size = (sizeof(lines) / sizeof(lines[0])) ;
    int i = 0;
    memset(lines, 0, sizeof(lines));

    lines[i] = strtok(buf, "\r\n");

    while (lines[i] != NULL && i < lines_size  ) {
        lines[++i] = strtok(NULL, "\r\n");
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
    while (!get_restserver_quit())
    {
        addrlen = sizeof(clientaddr);
        int nbytes = recvfrom(sock, msgbuf, MSGBUFSIZE - 1, 0, (struct sockaddr *) &clientaddr, &addrlen);

        if (nbytes < 0) {
            perror("recvfrom");
            continue;
        }

        if (nbytes < MSGBUFSIZE - 1)
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

