#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <task/task.h>
#include <adt/tst.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <zmq.h>
#include <assert.h>
#include <dbg.h>
#include <proxy.h>
#include <listener.h>

#include <register.h>


FILE *LOG_FILE = NULL;

static char *LEAVE_MSG = "{\"type\":\"leave\"}";
size_t LEAVE_MSG_LEN = 0;

char *UUID = "907F620B-BC91-4C93-86EF-512B71C2AE27";

enum
{
    HANDLER_STACK = 100 * 1024
};

void *listener_socket = NULL;

void from_handler_task(void *v);
int handler_deliver(int from_fd, char *buffer, size_t len);
void *handler_socket = NULL;

void our_free(void *data, void *hint)
{
    free(data);
}


void from_handler_task(void *socket)
{
    zmq_msg_t *inmsg = calloc(sizeof(zmq_msg_t), 1);
    char *data = NULL;
    size_t sz = 0;
    int fd = 0;
    int rc = 0;

    while(1) {
        zmq_msg_init(inmsg);

        rc = mqrecv(socket, inmsg, 0);
        check(rc == 0, "Receive on handler socket failed.");

        data = (char *)zmq_msg_data(inmsg);
        sz = zmq_msg_size(inmsg);

        if(data[sz-1] != '\0') {
            log_err("Last char from handler is not 0 it's %d, fix your backend.", data[sz-1]);
        } if(data[sz-2] == '\0') {
            log_err("You have two \0 ending your message, that's bad.");
        } else {
            int end = 0;
            int ok = sscanf(data, "%u%n", &fd, &end);
            debug("MESSAGE from handler: %s for fd: %d, nread: %d, len: %d, final: %d, last: %d",
                    data, fd, end, sz, sz-end-1, (data + end)[sz-end-1]);

            if(ok <= 0 || end <= 0) {
                log_err("Message didn't start with a ident number.");
            } else if(!Register_exists(fd)) {
                log_err("Ident %d is no longer connected.", fd);

                if(handler_deliver(fd, LEAVE_MSG, LEAVE_MSG_LEN) == -1) {
                    log_err("Can't tell handler %d died.", fd);
                }
            } else {
                if(Listener_deliver(fd, data+end, sz-end-1) == -1) {
                    log_err("Error sending to listener %d, closing them.");
                    Register_disconnect(fd);
                }
            }
        }
    }

    return;
error:
    taskexitall(1);
}





int handler_deliver(int from_fd, char *buffer, size_t len)
{
    int rc = 0;
    zmq_msg_t *msg = NULL;
    char *msg_buf = NULL;
    int msg_size = 0;
    size_t sz = 0;

    msg = calloc(sizeof(zmq_msg_t), 1);
    msg_buf = NULL;

    check(msg, "Failed to allocate 0mq message to send.");

    rc = zmq_msg_init(msg);
    check(rc == 0, "Failed to initialize 0mq message to send.");

    sz = strlen(buffer) + 32;
    msg_buf = malloc(sz);
    check(msg_buf, "Failed to allocate message buffer for handler delivery.");

    msg_size = snprintf(msg_buf, sz, "%d %.*s", from_fd, len, buffer);
    check(msg_size > 0, "Message too large, killing it.");

    rc = zmq_msg_init_data(msg, msg_buf, msg_size, our_free, NULL);
    check(rc == 0, "Failed to init 0mq message data.");

    rc = zmq_send(handler_socket, msg, 0);
    check(rc == 0, "Failed to deliver 0mq message to handler.");

    if(msg) free(msg);
    return 0;

error:
    if(msg) free(msg);
    if(msg_buf) free(msg_buf);
    return -1;
}



void taskmain(int argc, char **argv)
{
	int cfd, fd;
    int rport;
	char remote[16];
    LEAVE_MSG_LEN = strlen(LEAVE_MSG);
    int rc = 0;
    LOG_FILE = stderr;
	
    check(argc == 4, "usage: server localport handlerq listenerq");
    char *handler_spec = argv[2];
    char *listener_spec = argv[3];

    mqinit(2);
    Proxy_init("127.0.0.1", 80);

	int port = atoi(argv[1]);
    check(port > 0, "Can't bind to the given port: %s", argv[1]);

    handler_socket = mqsocket(ZMQ_PUB);
    rc = zmq_setsockopt(handler_socket, ZMQ_IDENTITY, UUID, strlen(UUID));
    check(rc == 0, "Failed to set handler socket %s identity %s", handler_spec, UUID);

    debug("Binding handler PUB socket %s with identity: %s", argv[2], UUID);

    rc = zmq_bind(handler_socket, handler_spec);
    check(rc == 0, "Can't bind handler socket: %s", handler_spec);

    listener_socket = Listener_init(listener_spec, "");  // TODO: add uuid
    check(listener_socket, "Failed to create listener socket.");

	fd = netannounce(TCP, 0, port);
    check(fd >= 0, "Can't announce on TCP port %d", port);

    debug("Starting server on port %d", port);
    taskcreate(from_handler_task, listener_socket, HANDLER_STACK);

	fdnoblock(fd);

	while((cfd = netaccept(fd, remote, &rport)) >= 0){
		taskcreate(Listener_task, (void*)cfd, LISTENER_STACK);
	}



error:
    log_err("Exiting due to error.");
    taskexitall(1);
}
