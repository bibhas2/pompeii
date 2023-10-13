#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <netdb.h>
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

void Server::reset() {
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

    for (auto& c : client_state) {
        c.reset();
    }
}

void populate_fd_set(EventLoop &loop, fd_set &read_fd_set, fd_set &write_fd_set) {
    FD_ZERO(&read_fd_set);
    FD_ZERO(&write_fd_set);
    
    for (auto& server : loop.server_state) {
        if (server.in_use()) {
            //Set the server socket
            FD_SET(server.server_socket, &read_fd_set);
            
            //Set the clients
            for (auto& c : server.client_state) {
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

    for (auto& client : loop.client_state) {
        if (client.in_use()) {
            /*
            * We need to enable read select no matter what
            * the value of read_write_flag is. This is 
            * because an orderly disconnect by the server
            * is signalled using a failed read and we need
            * to know that.
            */
            FD_SET(client.fd, &read_fd_set);

            /*
            * Enable write select if writing is scheduled, or,
            * an asynchronous connection is initiated but hasn't completed yet.
            * A completed connection is indicated by a write event.
            */
            if ((client.read_write_flag & RW_STATE_WRITE) || (client.is_connected == false)) {
                FD_SET(client.fd, &write_fd_set);
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
    
    const char *buffer_start = cli_state.read_buffer + cli_state.read_completed;

    int bytes_read = read(cli_state.fd,
                         (void*) buffer_start,
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

    if (cli_state.handler) {
        cli_state.handler->on_read(cli_state, buffer_start, bytes_read);
    }
    if (server.handler) {
        server.handler->on_read(server, cli_state, buffer_start, bytes_read);
    }

    if (cli_state.read_completed == cli_state.read_length) {
        cli_state.read_write_flag = cli_state.read_write_flag & (~RW_STATE_READ);
        if (cli_state.handler) {
            cli_state.handler->on_read_completed(cli_state);
        }        
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
    
    const char *buffer_start = cli_state.write_buffer + cli_state.write_completed;
    int bytes_written = write(cli_state.fd,
                             (void*) buffer_start,
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
    
    if (cli_state.handler) {
        cli_state.handler->on_write(cli_state, buffer_start, bytes_written);
    }
    if (server.handler) {
        server.handler->on_write(server, cli_state, buffer_start, bytes_written);
    }
    
    if (cli_state.write_completed == cli_state.write_length) {
        cli_state.read_write_flag &= ~RW_STATE_WRITE;
        if (cli_state.handler) {
            cli_state.handler->on_write_completed(cli_state);
        }
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

void dispatch_server_event(Server &state, fd_set &read_fd_set, fd_set &write_fd_set) {
    //Make sense out of the event
    if (FD_ISSET(state.server_socket, &read_fd_set)) {
        _trace("Client is connecting...");
        int client_fd = accept(state.server_socket, NULL, NULL);
        
        DIE(client_fd, "accept() failed.");
        
        bool added = state.add_client_fd(client_fd);
        
        if (!added) {
            _trace("Too many clients. Disconnecting...");

            close(client_fd);
            state.remove_client_fd(client_fd);

            return;
        }
        
        int status = fcntl(client_fd, F_SETFL, O_NONBLOCK);
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
                    _trace("Client has disconnected. Status: %d", status);

                    if (state.handler) {
                        state.handler->on_client_disconnect(state, c);
                    }

                    _trace("Closing client socket: %d", c.fd);
                    close(c.fd);
                    state.remove_client_fd(c.fd);
                }
            }
            
            if (c.fd < 0) {
                //Client has been disconnected.
                //No need to proceed to read from the client.
                continue;
            }
            
            if (FD_ISSET(c.fd, &write_fd_set)) {
                int status = handle_client_read(state, c);

                if (status < 1) {
                    //Client disconnected
                    _trace("Client has disconnected. Status: %d", status);

                    if (state.handler) {
                        state.handler->on_client_disconnect(state, c);
                    }

                    _trace("Closing client socket: %d", c.fd);
                    close(c.fd);
                    state.remove_client_fd(c.fd);
                }
            }
        }
    }
}

int handle_server_read(Client cli_state) {
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

    const char *buffer_start = cli_state.write_buffer + cli_state.write_completed;
    int bytes_written = write(cli_state.fd,
            (void*) buffer_start,
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
        //Server has disconnected in an unexpected manner. 
        //This is different than an orderly disconnected by the server.
        //We convert this event to an error.
        return -1;
    }

    cli_state.write_completed += bytes_written;

    if (cli_state.handler) {
        cli_state.handler->on_write(cli_state, buffer_start, bytes_written);
    }

    if (cli_state.write_completed == cli_state.write_length) {
        //Write is completed. Cancel further write.
        cli_state.cancel_write();

        if (cli_state.handler) {
            cli_state.handler->on_write_completed(cli_state);
        }
    }

    return bytes_written;
}

int handle_server_write(Client &cli_state) {
    if (!(cli_state.read_write_flag & RW_STATE_READ)) {
        //Socket is not trying to read. Possibly a
		//server disconnect signal.
		char ch;

		int bytes_read = read(cli_state.fd,
			&ch, sizeof(char));

		if (bytes_read == 0) {
			_trace("Orderly disconnect detected.");
		} else {
			_trace("Unexpected out of band incoming data.");			
		}

        return -1;
    }

	//Make sure read buffer is setup
    assert(cli_state.read_buffer != NULL);

	//Make sure read is pending
    assert(cli_state.read_length > cli_state.read_completed);

    const char *buffer_start = cli_state.read_buffer + cli_state.read_completed;
    int bytes_read = read(cli_state.fd,
            (void*) buffer_start,
            cli_state.read_length - cli_state.read_completed);

    _trace("Read %d of %d bytes", bytes_read, cli_state.read_length);

    if (bytes_read < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }

        //Read will block. Not an error.
        return 0;
    }

    if (bytes_read == 0) {
        //Server has disconnected unexpectedly. We convert that to an error.
        return -1;
    }

    cli_state.read_completed += bytes_read;
	
	bool read_finished = cli_state.read_completed == cli_state.read_length;

    if (cli_state.handler) {
        cli_state.handler->on_read(cli_state, buffer_start, bytes_read);
    }

    if (read_finished) {
        //Read is completed. Cancel further read.
		cli_state.cancel_read();

        if (cli_state.handler) {
            cli_state.handler->on_read_completed(cli_state);
        }
	}

	return bytes_read;
}

void dispatch_client_event(Client &client, fd_set &read_fd_set, fd_set &write_fd_set) {
    if (FD_ISSET(client.fd, &read_fd_set)) {
        int status = handle_server_write(client);
        
        if (status < 1) {
            close(client.fd);
            client.fd = -1;

            _trace("Orderly server disconnect.");

            if (client.handler) {
                client.handler->on_server_disconnect(client);
            }

            client.reset();

            return;
        }
    }

    if (!client.in_use()) {
        //No point going forward
        return;
    }

    if (FD_ISSET(client.fd, &write_fd_set)) {
        if (client.is_connected == false) {
            //Connection is now complete. See if it was successful
            int valopt; 
            socklen_t lon = sizeof(int); 

            if (getsockopt(client.fd, SOL_SOCKET, SO_ERROR, (void*)(&valopt), &lon) < 0) { 
                perror("Error in getsockopt()");

                return;
            }

            //Check the value of valopt
            if (valopt) {
                //Connection failed
                _trace("Error connecting to server: %s.\n", strerror(valopt));
                
                close(client.fd);
                client.fd = -1;

                if (client.handler) {
                    client.handler->on_server_connect_failed(client);
                }

                client.reset();
            } else {
                //Connection was successful
                client.is_connected = true;
                _trace("Asynchronous connection completed.");

                if (client.handler) {
                    client.handler->on_server_connect(client);
                }
            }
        } else {
            int status = handle_server_read(client);

            if (status < 1) {
                _trace("Unexpected server disconnect.");
                
                close(client.fd);
                client.fd = -1;

                if (client.handler) {
                    client.handler->on_server_disconnect(client);
                }

                client.reset();
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
    
    fd_set read_fd_set, write_fd_set;
    struct timeval timeout;
    
    while (continue_loop) {
        populate_fd_set(*this, read_fd_set, write_fd_set);
                
        timeout.tv_sec = idle_timeout;
        timeout.tv_usec = 0;
        
        int num_events = select(
                               FD_SETSIZE,
                               &read_fd_set,
                               &write_fd_set,
                               NULL,
                               idle_timeout > 0 ? &timeout : NULL);
        
        if (num_events < 0 && errno == EINTR) {
            //A signal was handled
            continue;
        }

        DIE(num_events, "select() failed.");
        
        if (num_events == 0) {
            _trace("select() timed out.");
            for (auto& s : server_state) {                
                if (s.in_use() && s.handler) {
                    s.handler->on_timeout(s);
                }
            }

            for (auto& c : client_state) {
                if (c.in_use() && c.handler) {
                    c.handler->on_timeout(c);
                }
            }
            
            continue;
        }
        
        for (auto& s : server_state) {
            if (s.in_use()) {
                dispatch_server_event(s, read_fd_set, write_fd_set);
            }
        }

        for (auto& c : client_state) {
            if (c.in_use()) {
                dispatch_client_event(c, read_fd_set, write_fd_set);
            }
        }
    }
}

void Server::start(int port) {
    _trace("Starting server at port: %d", port);

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

void Client::schedule_read(const char *buffer, size_t length) {
    assert(fd >= 0); //Bad socket?
    assert((read_write_flag & RW_STATE_READ) == 0); //Already reading?
    
    read_buffer = buffer;
    read_length = length;
    read_completed = 0;
    read_write_flag |= RW_STATE_READ;
    
    _trace("Scheduling read for socket: %d", fd);
}

void Client::schedule_write(const char *buffer, size_t length) {
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

void EventLoop::add_server(int port, std::shared_ptr<ServerEventHandler> handler) {
    for (auto& s : server_state) {
        if (!s.in_use()) {
            s.handler = handler;

            s.start(port);

            return;
        }
    }
}

void EventLoop::end() {
    continue_loop = 0;
}

int client_make_connection(Client &cstate, const char *host, int port) {
	_trace("Connecting to %s:%d", host, port);

	char port_str[128];

	snprintf(port_str, sizeof(port_str), "%d", port);

	struct addrinfo hints, *res;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_INET;
	hints.ai_socktype = SOCK_STREAM;

	int status = getaddrinfo(host, port_str, &hints, &res);

	if (status < 0 || res == NULL) {
		_trace("Failed to resolve address: %s", host);
		
        return -1;
	}

	int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

    if (sock < 0) {
        _trace("Failed to open socket.");
        
        freeaddrinfo(res);

        return -1;
    }

    //Make the socket non-blocking
	status = fcntl(sock, F_SETFL, O_NONBLOCK);

    if (status < 0) {
        _trace("Failed to set non blocking mode for socket.");

        freeaddrinfo(res);

        return -1;
    }

	status = connect(sock, res->ai_addr, res->ai_addrlen);

    //Don't need this any more
	freeaddrinfo(res);

	if (status < 0 && errno != EINPROGRESS) {
		perror("Failed to connect to port.");

		close(sock);

		return -1;
	}

	cstate.fd = sock;

	return cstate.fd;
}

int EventLoop::add_client(const char *host, int port, std::shared_ptr<ClientEventHandler> handler) {
    //Find a free client slot
    for (auto& c : client_state) {
        if (!c.in_use()) {
            c.reset();

            c.handler = handler;

            return client_make_connection(c, host, port);
        }
    }

    //No more room
    return -1;
}

}