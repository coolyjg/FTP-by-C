#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <memory.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <errno.h>
#include "connector.h"
#include "security.h"

#define REDIRECT
#define MAXEVENTS 64

char defaultDir[50], defaultPort[10],defaultMaxcon[10];

static int unblockSocket(int sockfd)
{
    int flags;
    flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        perror ("fcntl get");
        return -1;
    }
    flags |= O_NONBLOCK;
    flags = fcntl(sockfd, F_SETFL, flags);
    if (flags == -1) {
        perror("fcntl set");
        return -1;
    }
    return 0;
}

static int createAndBind(char *port)
{
    struct sockaddr_in addr;
    int sockfd;
    if ((sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1) {
        printf("Error socket(): %s(%d)\n", strerror(errno), errno);
        return 1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(c2i(defaultPort));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
		printf("Error bind(): %s(%d)\n", strerror(errno), errno);
		return 1;
    }
    return sockfd;
}

int main (int argc, char *argv[])
{
    int sockfd, flag;
    int efd, i;
    pid_t pid;
    struct epoll_event event;
    struct epoll_event *events;
    Connector* connList;
    SecurityGroup* securitygroup;

    #ifdef REDIRECT
    FILE *log;
    if((log=freopen("sever_log.txt","w+",stdout))==NULL) exit(-1);
    #endif

    strcpy(defaultPort, "21");
    strcpy(defaultDir, "/tmp");
	strcpy(defaultMaxcon,"128");

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-port")) strcpy(defaultPort, argv[++i]);
        else if (!strcmp(argv[i], "-root")) strcpy(defaultDir, argv[++i]);
		else if (!strcmp(argv[i], "-maxcon")) strcpy(defaultMaxcon, argv[++i]);
        else {
            printf("wrong arguments![%s]", argv[i]);
            return 1;
        }
    }
    printf("server on port[%s] at root[%s] with maxconnection [%s]\n", defaultPort, defaultDir,defaultMaxcon);


    securitygroup = loadSecurity("FireWall.txt");
    connList = createConnectorList();
    sockfd = createAndBind(argv[1]);
    if (sockfd == -1)
        abort();
    flag = unblockSocket(sockfd);
    if (flag == -1)
        abort();
    flag = listen(sockfd, c2i(defaultMaxcon));
    if (flag == -1) {
        perror("listen");
        abort();
    }
    efd = epoll_create1(0);
    if (efd == -1) {
        perror("epoll_create");
        abort();
    }

    event.data.fd = sockfd;
    event.events = EPOLLIN | EPOLLET;
    flag = epoll_ctl(efd, EPOLL_CTL_ADD, sockfd, &event);
    if (flag == -1) {
        perror("epoll_ctl");
        abort();
    }

    events = calloc(MAXEVENTS, sizeof event);

    while (1) {
        #ifdef REDIRECT
        fclose(stdout);
        if((log=freopen("sever_log.txt","a+",stdout))==NULL) exit(-1);
        #endif
        int n, i;
        n = epoll_wait(efd, events, MAXEVENTS, -1);
        for (i = 0; i < n; i++) { //连接正常关闭事件
            if ((events[i].events & EPOLLERR) ||
                (events[i].events & EPOLLHUP) ||
                (!(events[i].events & EPOLLIN))) {
                deleteConnector(connList, events[i].data.fd);
                continue;
            }

            else if (sockfd == events[i].data.fd) { //有新连接
                /* New connections */
                while (1) {
                    struct sockaddr addr;
                    socklen_t len;
                    int connfd;
                    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
                    len = sizeof addr;
                    connfd = accept(sockfd, &addr, &len);
                    if (connfd == -1) {
                        if ((errno == EAGAIN) ||
                            (errno == EWOULDBLOCK)) {
                            break;
                        }
                        else {
                            perror ("accept");
                            break;
                        }
                    }
                    flag = getnameinfo(&addr, len,
                                       hbuf, sizeof hbuf,
                                       sbuf, sizeof sbuf,
                                       NI_NUMERICHOST | NI_NUMERICSERV);
                    if (flag == 0) {
                        if(securitycheck(securitygroup,hbuf)){
                            printf("Accepted connection on descriptor %d "
                                   "(host=%s, port=%s)\n", connfd, hbuf, sbuf);
                        }
                        else{
                            printf("Accepted connection on descriptor %d "
                                   "(host=%s, port=%s) is not secure and has been aborted!\n", connfd, hbuf, sbuf);
                            break;
                        }


                    }
                    flag = unblockSocket(connfd);
                    if (flag == -1)
                        abort();
                    event.data.fd = connfd;
                    event.events = EPOLLIN | EPOLLET;
                    appendConnector(connList, connfd, defaultDir);
                    flag = write(connfd, S220, strlen(S220));
                    flag = epoll_ctl(efd, EPOLL_CTL_ADD, connfd, &event);
                    if (flag == -1) {
                        perror("epoll_ctl");
                        abort();
                    }
                }
                continue;
            }
            else {
                int done = 0;
                ssize_t count;
                char buf[512];
                while (1) {
                    count = read(events[i].data.fd, buf, sizeof buf);
                    if (count == -1) {
                        if (errno != EAGAIN) {
                            perror("read");
                            done = 1;
                        }
                        break;
                    }
                    else if (count == 0) {
                        done = 1;
                        break;
                    }
                    buf[count] = '\0';
                    printf("%s", buf);
                }
                if (!done) {
                    responseClient(connList, events[i].data.fd, buf);
                }
                else {
                    deleteConnector(connList, events[i].data.fd);
                }
            }
        }
    }
    free(events);
    close(sockfd);
    return EXIT_SUCCESS;
}
