#include "taskimpl.h"
#include <zmq.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <stdio.h>
#include <signal.h>
#include <sys/socket.h>

enum
{
    // TODO: base how many we can handle on the rlimit possible
    MAXFD = 1024 * 10, FDSTACK= 100 * 1024
};


static zmq_pollitem_t pollfd[MAXFD];
static Task *polltask[MAXFD];
static int npollfd;
static int startedfdtask;
static Tasklist sleeping;
static int sleepingcounted;
static uvlong nsec(void);

void *ZMQ_CTX = NULL;


#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

void mqinit(int threads)
{
    if(ZMQ_CTX == NULL) {
        ZMQ_CTX = zmq_init(threads);

        if(!ZMQ_CTX) {
            printf("Error setting up 0mq.\n");
            exit(1);
        }
    }
}

void
fdtask(void *v)
{
    int i, ms;
    Task *t;
    uvlong now;

    tasksystem();
    taskname("fdtask");

    for(;;){
        /* let everyone else run */
        while(taskyield() > 0)
            ;
        /* we're the only one runnable - poll for i/o */
        errno = 0;
        taskstate("poll");
        if((t=sleeping.head) == nil)
            ms = -1;
        else{
            /* sleep at most 5s */
            now = nsec();
            if(now >= t->alarmtime)
                ms = 0;
            else if(now+5*1000*1000*1000LL >= t->alarmtime)
                ms = (t->alarmtime - now)/1000000;
            else
                ms = 5000;
        }

        if(zmq_poll(pollfd, npollfd, ms) < 0){
            if(errno == EINTR) {
                continue;
            }

            fprint(2, "poll: %d:%s\n", errno, strerror(errno));
            taskexitall(0);
        }

        /* wake up the guys who deserve it */
        for(i=0; i<npollfd; i++){
            while(i < npollfd && pollfd[i].revents){
                taskready(polltask[i]);
                --npollfd;
                pollfd[i] = pollfd[npollfd];
                polltask[i] = polltask[npollfd];
            }
        }
        
        now = nsec();
        while((t=sleeping.head) && now >= t->alarmtime){
            deltask(&sleeping, t);
            if(!t->system && --sleepingcounted == 0)
                taskcount--;
            taskready(t);
        }
    }
}

int tasknuke(int id)
{
    int i = 0;
    // TODO: this should shuffle ram around if possible or something
    for(i = 0; i < npollfd; i++) {
        if(polltask[i] && polltask[i]->id == id) {
            pollfd[i].fd = -1;
            pollfd[i].socket = NULL;
            pollfd[i].events = 0;
            pollfd[i].revents = 0;
            polltask[i] = NULL;
            return 0;
        }
    }

    return -1;
}

int taskwaiting()
{
    // TODO: until we do the cleanup of pollfd better we have
    // to count them this way
    int count = 0;
    int i = 0;

    for(i = 0; i < npollfd; i++) {
        if(polltask[i] && pollfd[i].events) {
            count++;
        }
    }

    return count;
}

uint
taskdelay(uint ms)
{
    uvlong when, now;
    Task *t;
    
    if(!startedfdtask){
        startedfdtask = 1;
        taskcreate(fdtask, 0, FDSTACK);
    }

    now = nsec();
    when = now+(uvlong)ms*1000000;
    for(t=sleeping.head; t!=nil && t->alarmtime < when; t=t->next)
        ;

    if(t){
        taskrunning->prev = t->prev;
        taskrunning->next = t;
    }else{
        taskrunning->prev = sleeping.tail;
        taskrunning->next = nil;
    }
    
    t = taskrunning;
    t->alarmtime = when;
    if(t->prev)
        t->prev->next = t;
    else
        sleeping.head = t;
    if(t->next)
        t->next->prev = t;
    else
        sleeping.tail = t;

    if(!t->system && sleepingcounted++ == 0)
        taskcount++;
    taskswitch();

    return (nsec() - now)/1000000;
}

int
_wait(void *socket, int fd, int rw)
{
    int bits;

    if(!startedfdtask) {
        startedfdtask = 1;
        taskcreate(fdtask, 0, FDSTACK);
    }

    if(npollfd >= MAXFD){
        errno = EBUSY;
        return -1;
    }
    
    taskstate("wait %d:%s", socket ? (int)socket : fd, 
            rw=='r' ? "read" : rw=='w' ? "write" : "error");

    bits = 0;
    switch(rw){
    case 'r':
        bits |= ZMQ_POLLIN;
        break;
    case 'w':
        bits |= ZMQ_POLLOUT;
        break;
    }

    polltask[npollfd] = taskrunning;
    pollfd[npollfd].fd = fd;
    pollfd[npollfd].socket = socket;
    pollfd[npollfd].events = bits;
    pollfd[npollfd].revents = 0;
    npollfd++;

    taskswitch();

    return 0;
}

int fdwait(int fd, int rw)
{
    return _wait(NULL, fd, rw);
}

void *mqsocket(int type)
{
    return zmq_socket(ZMQ_CTX, type);
}

int mqwait(void *socket, int rw)
{
    return _wait(socket, -1, rw);
}

int mqrecv(void *socket, zmq_msg_t *msg, int flags)
{
    if(mqwait(socket, 'r') == -1) {
        return -1;
    }

    return zmq_recv(socket, msg, flags);
}

int mqsend(void *socket, zmq_msg_t *msg, int flags)
{
    if(mqwait(socket, 'w') == -1) {
        return -1;
    }

    return zmq_send(socket, msg, flags);
}


/* Like fdread but always calls fdwait before reading. */
int
fdread1(int fd, void *buf, int n)
{
    int m;
    
    do {
        if(fdwait(fd, 'r') == -1) {
            return -1;
        }
    } while((m = read(fd, buf, n)) < 0 && errno == EAGAIN);

    return m;
}

int
fdrecv1(int fd, void *buf, int n)
{
    int m;
    
    do {
        if(fdwait(fd, 'r') == -1) {
            return -1;
        }
    } while((m = recv(fd, buf, n, MSG_NOSIGNAL)) < 0 && errno == EAGAIN);

    return m;
}

int
fdread(int fd, void *buf, int n)
{
    int m;

    while((m=read(fd, buf, n)) < 0 && errno == EAGAIN) {
        if(fdwait(fd, 'r') == -1) {
            return -1;
        }
    }
    return m;
}

int
fdrecv(int fd, void *buf, int n)
{
    int m;

    while((m=recv(fd, buf, n, MSG_NOSIGNAL)) < 0 && errno == EAGAIN) {
        if(fdwait(fd, 'r') == -1) {
            return -1;
        }
    }
    return m;
}

int
fdwrite(int fd, void *buf, int n)
{
    int m, tot;
    
    for(tot = 0; tot < n; tot += m){
        while((m=write(fd, (char*)buf+tot, n-tot)) < 0 && errno == EAGAIN) {
            if(fdwait(fd, 'w') == -1) {
                return -1;
            }
        }

        if(m < 0) return m;
        if(m == 0) break;
    }
    return tot;
}

int
fdsend(int fd, void *buf, int n)
{
    int m, tot;
    
    for(tot = 0; tot < n; tot += m){
        while((m=send(fd, (char*)buf+tot, n-tot, MSG_NOSIGNAL)) < 0 && errno == EAGAIN) {
            if(fdwait(fd, 'w') == -1) {
                return -1;
            }
        }

        if(m < 0) return m;
        if(m == 0) break;
    }
    return tot;
}

int
fdnoblock(int fd)
{
#ifdef SO_NOSIGPIPE
    int set = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (void *)&set, sizeof(int));
#endif
    
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL)|O_NONBLOCK);
}

static uvlong
nsec(void)
{
    struct timeval tv;

    if(gettimeofday(&tv, 0) < 0)
        return -1;
    return (uvlong)tv.tv_sec*1000*1000*1000 + tv.tv_usec*1000;
}


