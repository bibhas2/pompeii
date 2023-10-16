#pragma once

#include <memory>

#define MAX_CLIENTS 5
#define MAX_SERVERS 5

namespace pompeii {
struct Client;
struct Server;

const uint32_t RW_STATE_NONE = 0;
const uint32_t RW_STATE_READ = 2;
const uint32_t RW_STATE_WRITE = 4;

struct ClientEventHandler {
    virtual void on_server_connect(Client&) {};
    virtual void on_server_connect_failed(Client&) {};
    virtual void on_server_disconnect(Client&) {};
    virtual void on_read(Client&, const char* buffer, int bytes_read) {};
    virtual void on_write(Client&, const char* buffer, int bytes_read) {};
    virtual void on_read_completed(Client&) {};
    virtual void on_write_completed(Client&) {};
    virtual void on_read(Server&, Client&, const char* buffer, int bytes_read) {};
    virtual void on_write(Server&, Client&, const char* buffer, int bytes_read) {};
    virtual void on_read_completed(Server&, Client&) {};
    virtual void on_write_completed(Server&, Client&) {};
    virtual void on_timeout(Client&) {};
};

struct ServerEventHandler {
    virtual void on_loop_start(Server&) {};
    virtual void on_loop_end() {};
    virtual void on_timeout(Server&) {};
    virtual void on_client_connect(Server&, Client&) {};
    virtual void on_client_disconnect(Server&, Client&) {};
    virtual void on_read(Server&, Client&, const char* buffer, int bytes_read) {};
    virtual void on_write(Server&, Client&, const char* buffer, int bytes_read) {};
    virtual void on_read_completed(Server&, Client&) {};
    virtual void on_write_completed(Server&, Client&) {};
};

struct Client {
	int fd;
	const char *read_buffer;
    size_t read_length;
    size_t read_completed;
    const char *write_buffer;
    size_t write_length;
    size_t write_completed;
    char host[128];
	int port;
    uint32_t read_write_flag;
    bool is_connected;
    std::shared_ptr<ClientEventHandler> handler;

    Client();
    void reset();

    bool in_use() {
        return fd >= 0;
    }

    void schedule_read(const char *buffer, size_t length);
    void schedule_write(const char *buffer, size_t length);
    void cancel_read();
    void cancel_write();
    template <class H>
    std::shared_ptr<H> get_handler() {
        return std::dynamic_pointer_cast<H>(handler);
    }
};

struct Server {
    int server_socket;
	Client client_state[MAX_CLIENTS];
    std::shared_ptr<ServerEventHandler> handler;

    Server();
    ~Server();
    void reset();
    bool in_use() {
        return server_socket >= 0;
    }
    void disconnect_clients();
    void disconnect_client(Client &c);
    bool add_client_fd(int fd);
    bool remove_client_fd(int fd);

    void start(int port);
    template <class H>
    std::shared_ptr<H> get_handler() {
        return std::dynamic_pointer_cast<H>(handler);
    }
};

struct EventLoop {
    Server server_state[MAX_SERVERS];
    Client client_state[MAX_CLIENTS];

    bool continue_loop;
    int idle_timeout; //Timeout in seconds. -1 for no timeout.

    EventLoop();
    void start();
    void end();
    void add_server(int port, std::shared_ptr<ServerEventHandler> handler);
    int add_client(const char *host, int port, std::shared_ptr<ClientEventHandler> handler);
};

void enable_trace(int flag);

}
