#include <pompeii.h>
#include <string_view>
#include <iostream>

struct MyClient : public pompeii::ClientEventHandler {
    int num_sent = 0;
    char read_buff[256];

    ~MyClient() {
        printf("MyClient getting cleaned up.\n");
    }
    void greet(pompeii::Client& c) {
        ++num_sent;

        const char* data = "Hello World\n";

        c.schedule_write(data, strlen(data));
    }
    void on_write_completed(pompeii::Client& c) {
        if (num_sent < 10) {
            greet(c);
        } else {
            c.schedule_read(read_buff, sizeof(read_buff));
        }
    }
    void on_read(pompeii::Client& c, const char* buffer, int bytes_read) {
        auto sv = std::string_view(buffer, bytes_read);

        std::cout << sv << std::endl;
    }
};

struct MyServer : public pompeii::ServerEventHandler {
    void on_client_connect(pompeii::Server& s, pompeii::Client& c) {
        printf("Client connected. Socket: %d\n", c.fd);

        auto mc = std::make_shared<MyClient>();

        c.handler = mc;

        mc->greet(c);
    }
    void on_client_disconnect(pompeii::Server& s, pompeii::Client& c) {
        printf("Client disconnected. Socket: %d\n", c.fd);
    }
};

int main() {
    pompeii::enable_trace(1);

    pompeii::EventLoop loop;

    loop.add_server(9080, std::make_shared<MyServer>());

    loop.start();
}