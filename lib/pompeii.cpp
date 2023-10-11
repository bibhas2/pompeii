#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>

#include "pompeii.h"

#define DIE(value, message) if (value < 0) {perror(message); exit(value);}

namespace pompeii {

static int trace_on = 0;

void
_trace(const char* fmt, ...) {
    if (trace_on == 0) {
        return;
    }
    
    va_list ap;
    
    va_start(ap, fmt);
    
    printf("INFO: ");
    vprintf(fmt, ap);
    printf("\n");
}

void
enable_trace(int flag) {
    trace_on = flag;
}

Client::Client() {
    reset();
}

void Client::reset() {
    fd = -1;
    write_buffer = NULL;
    write_length = 0;
    write_completed = 0;
    read_buffer = NULL;
    read_length = 0;
    read_completed = 0;
    read_write_flag = RW_STATE_NONE;
    is_connected = false;

    handler.reset();
}

Server::Server() {
    reset();
}

Server::Server(int port) {
    reset();

    this->port = port;
}

void Server::reset() {
    port = -1;
    server_socket = -1;

    for (auto& c : client_state) {
        c.reset();
    }

    handler.reset();
}

Server::~Server() {
    disconnect_clients();
}

EventLoop::EventLoop() {
    continue_loop = false;
    idle_timeout = 0;

    for (auto& s : server_state) {
        s.reset();
    }
}

void populate_fd_set(EventLoop &loop, fd_set &read_fd_set, fd_set &write_fd_set) {
    FD_ZERO(&read_fd_set);
    FD_ZERO(&write_fd_set);
    
    for (auto& state : loop.server_state) {
        if (state.in_use()) {
            //Set the server socket
            FD_SET(state.server_socket, &read_fd_set);
            
            //Set the clients
            for (auto& c : state.client_state) {
                if (!c.in_use()) {
                    continue;
                }
                
                if (c.read_write_flag & RW_STATE_READ) {
                    FD_SET(c.fd, &read_fd_set);
                }
                if (c.read_write_flag & RW_STATE_WRITE) {
                    FD_SET(c.fd, &write_fd_set);
                }
            }
        }
    }
}

void Server::disconnect_clients() {
    for (auto& c : client_state) {
        if (c.in_use()) {
            close(c.fd);

            c.reset();
        }
    }
}

bool Server::add_client_fd(int fd) {
    for (auto& c : client_state) {
        if (!c.in_use()) {
            c.fd = fd;

            if (handler) {
                handler->on_client_connect(*this, c);
            }

            return true;
        }
    }

    //We have no room for more clients
    return false;
}

bool Server::remove_client_fd(int fd) {
    for (auto& c : client_state) {
        if (c.in_use() && c.fd == fd) {
            c.reset();

            return true;
        }
    }

    //Not found!
    return false;
}

int handle_client_write(Server& server, Client &cli_state) {
    if (!(cli_state.read_write_flag & RW_STATE_READ)) {
        _trace("Socket is not trying to read.");
        
        return -1;
    }
    if (cli_state.read_buffer == NULL) {
        _trace("Read buffer not setup.");
        
        return -1;
    }
    if (cli_state.read_length == cli_state.read_completed) {
        _trace("Read was already completed.");
        
        return -1;
    }
    
    char *buffer_start = cli_state.read_buffer + cli_state.read_completed;

    int bytes_read = read(cli_state.fd,
                         buffer_start,
                         cli_state.read_length - cli_state.read_completed);
    
    _trace("Read %d of %d bytes", bytes_read, cli_state.read_length);
    
    if (bytes_read < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }

        //Read will block. Not an error.
        _trace("Read block detected.");

        return 0;
    }

    if (bytes_read == 0) {
        //Client has disconnected. We convert that to an error.
        return -1;
    }
    
    cli_state.read_completed += bytes_read;
    
    if (server.handler) {
        server.handler->on_read(server, cli_state, buffer_start, bytes_read);
    }

    if (cli_state.read_completed == cli_state.read_length) {
        cli_state.read_write_flag = cli_state.read_write_flag & (~RW_STATE_READ);
        
        if (server.handler) {
            server.handler->on_read_completed(server, cli_state);
        }
    }
    
    return bytes_read;
}

int handle_client_read(Server& server, Client &cli_state) {
    if (!(cli_state.read_write_flag & RW_STATE_WRITE)) {
        _trace("Socket is not trying to write.");
        
        return -1;
    }
    if (cli_state.write_buffer == NULL) {
        _trace("Write buffer not setup.");
        
        return -1;
    }
    if (cli_state.write_length == cli_state.write_completed) {
        _trace("Write was already completed.");
        
        return -1;
    }
    
    char *buffer_start = cli_state.write_buffer + cli_state.write_completed;
    int bytes_written = write(cli_state.fd,
                             buffer_start,
                             cli_state.write_length - cli_state.write_completed);
    
    _trace("Written %d of %d bytes", bytes_written, cli_state.write_length);
    
    if (bytes_written < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        //Write will block. Not an error.
        _trace("Write block detected.");
        
        return 0;
    }

    if (bytes_written == 0) {
        //Client has disconnected. We convert that to an error.
        return -1;
    }
    
    cli_state.write_completed += bytes_written;
    
    if (server.handler) {
        server.handler->on_write(server, cli_state, buffer_start, bytes_written);
    }
    
    if (cli_state.write_completed == cli_state.write_length) {
        cli_state.read_write_flag &= ~RW_STATE_WRITE;
        
        if (server.handler) {
            server.handler->on_write_completed(server, cli_state);
        }
    }
    
    return bytes_written;
}

void Server::disconnect_client(Client &cli_state) {
    if (handler) {
        handler->on_client_disconnect(*this, cli_state);
    }

    close(cli_state.fd);
    remove_client_fd(cli_state.fd);
}

void dispatch_event(Server &state, fd_set &read_fd_set, fd_set &write_fd_set) {
    //Make sense out of the event
    if (FD_ISSET(state.server_socket, &read_fd_set)) {
        _trace("Client is connecting...");
        int clientFd = accept(state.server_socket, NULL, NULL);
        
        DIE(clientFd, "accept() failed.");
        
        int added = state.add_client_fd(clientFd);
        
        if (!added) {
            _trace("Too many clients. Disconnecting...");

            close(clientFd);
            state.remove_client_fd(clientFd);
        }
        
        int status = fcntl(clientFd, F_SETFL, O_NONBLOCK);
        DIE(status, "Failed to set non blocking mode for client socket.");
    } else {
        //Client wrote something or disconnected
        for (auto& c : state.client_state) {
            
            if (!c.in_use()) {
                //This slot is not in use
                continue;
            }
            
            if (FD_ISSET(c.fd, &read_fd_set)) {
                int status = handle_client_write(state, c);

                if (status < 1) {
                    //Client has disconnected
                    _trace("Client is finished. Status: %d", status);

                    if (state.handler) {
                        state.handler->on_client_disconnect(state, c);
                    }

                    close(c.fd);
                    state.remove_client_fd(c.fd);
                }
            }
            
            if (c.fd < 0) {
                //Client write event caused application to disconnect.
                continue;
            }
            
            if (FD_ISSET(c.fd, &write_fd_set)) {
                int status = handle_client_read(state, c);

                if (status < 1) {
                    //Client disconnected
                    _trace("Client is finished. Status: %d", status);

                    if (state.handler) {
                        state.handler->on_client_disconnect(state, c);
                    }

                    close(c.fd);
                    state.remove_client_fd(c.fd);
                }
            }
        }
    }
}

void EventLoop::start() {
    continue_loop = true;
    
    for (auto& s : server_state) {
        if (s.in_use() && s.handler) {
            s.handler->on_loop_start(s);
        }
    }
    
    fd_set readFdSet, writeFdSet;
    struct timeval timeout;
    
    while (continue_loop) {
        populate_fd_set(*this, readFdSet, writeFdSet);
                
        timeout.tv_sec = idle_timeout;
        timeout.tv_usec = 0;
        
        int numEvents = select(
                               FD_SETSIZE,
                               &readFdSet,
                               &writeFdSet,
                               NULL,
                               idle_timeout > 0 ? &timeout : NULL);
        
        if (numEvents < 0 && errno == EINTR) {
            //A signal was handled
            continue;
        }

        DIE(numEvents, "select() failed.");
        
        if (numEvents == 0) {
            _trace("select() timed out.");
            for (auto& s : server_state) {                
                if (s.in_use() && s.handler) {
                    s.handler->on_timeout(s);
                }
            }
            
            continue;
        }
        
        for (auto& s : server_state) {
            if (s.in_use()) {
                dispatch_event(s, readFdSet, writeFdSet);
            }
        }
    }
}

void Server::start() {
    int status;
    
    int sock = socket(PF_INET, SOCK_STREAM, 0);
    
    DIE(sock, "Failed to open socket.");
    
    status = fcntl(sock, F_SETFL, O_NONBLOCK);
    DIE(status, "Failed to set non blocking mode for server listener socket.");
    
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    
    struct sockaddr_in addr;
    
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    status = bind(sock, (struct sockaddr*) &addr, sizeof(addr));
    
    DIE(status, "Failed to bind to port.");
    
    _trace("Calling listen.");
    status = listen(sock, 10);
    _trace("listen returned.");
    
    DIE(status, "Failed to listen.");
    
    server_socket = sock;
}

void Client::schedule_read(char *buffer, size_t length) {
    assert(fd >= 0); //Bad socket?
    assert((read_write_flag & RW_STATE_READ) == 0); //Already reading?
    
    read_buffer = buffer;
    read_length = length;
    read_completed = 0;
    read_write_flag |= RW_STATE_READ;
    
    _trace("Scheduling read for socket: %d", fd);
}

void Client::schedule_write(char *buffer, size_t length) {
    assert(fd >= 0); //Bad socket?
    assert((read_write_flag & RW_STATE_WRITE) == 0); //Already writing?
    
    write_buffer = buffer;
    write_length = length;
    write_completed = 0;
    read_write_flag |= RW_STATE_WRITE;
    
    _trace("Scheduling write for socket: %d", fd);
}

void Client::cancel_read() {
    read_buffer = NULL;
    read_length = 0;
    read_completed = 0;
    read_write_flag &= ~RW_STATE_READ;

    _trace("Cancel read for socket: %d", fd);
}

void Client::cancel_write() {
    write_buffer = NULL;
    write_length = 0;
    write_completed = 0;
    read_write_flag &= ~RW_STATE_WRITE;

    _trace("Cancel write for socket: %d", fd);
}

void EventLoop::add_server(Server &state) {
    assert(state.server_socket >= 0);
    
    for (auto& s : server_state) {
        if (!s.in_use()) {
            s = state; //Copy things

            state.reset();
        }
    }
}

void EventLoop::end() {
    continue_loop = 0;
}

}