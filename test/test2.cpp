#include <pompeii.h>
#include <string_view>
#include <iostream>

struct MyServer;

struct MyClient : public pompeii::ClientEventHandler {
    char buff[256];
    MyServer& server_state;

    MyClient(MyServer& s) : server_state(s) {

    }

    ~MyClient() {
        printf("MyClient getting cleaned up.\n");
    }
    void prompt(pompeii::Client& c) {
        auto prompt = "> ";
        c.schedule_write(prompt, strlen(prompt));

        c.schedule_read(buff, sizeof(buff));
    }
    void on_write_completed(pompeii::Client& c) {
        
    }
    void on_read(pompeii::Client& c, const char* buffer, int bytes_read);
};

struct MyServer : public pompeii::ServerEventHandler {
    pompeii::EventLoop& loop;

    MyServer(pompeii::EventLoop& l) : loop(l) {

    }

    void on_client_connect(pompeii::Server& s, pompeii::Client& c) {
        printf("Client connected. Socket: %d\n", c.fd);

        auto mc = std::make_shared<MyClient>(*this);

        c.handler = mc;

        mc->prompt(c);
    }
};

void MyClient::on_read(pompeii::Client& c, const char* buffer, int bytes_read) {
        std::string_view cmd(buffer, bytes_read);

        std::cout << cmd;

        if (cmd.starts_with("shutdown")) {
            server_state.loop.end();
        }

        c.cancel_read();

        prompt(c);
}

int main() {
    pompeii::enable_trace(1);

    pompeii::EventLoop loop;

    loop.add_server(9080, std::make_shared<MyServer>(loop));

    loop.start();
}