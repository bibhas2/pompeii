#include <pompeii.h>
#include <string_view>
#include <iostream>

struct MyClient : public pompeii::ClientEventHandler {
    char buff[256];

    ~MyClient() {
        printf("MyClient getting cleaned up.\n");
    }
    void prompt(pompeii::Client& c) {
        auto prompt = "> ";
        c.schedule_write(prompt, strlen(prompt));

        c.schedule_read(buff, sizeof(buff));
    }

    void display_stats(pompeii::Server& s) {
        for (auto& c : s.client_state) {
            if (c.in_use()) {
                printf("Client: %d\n", c.fd);
                printf("Reading: %s\n", (c.read_write_flag & pompeii::RW_STATE_READ) ? "Y" : "N");
                printf("Writing: %s\n", (c.read_write_flag & pompeii::RW_STATE_WRITE) ? "Y" : "N");
            }
        }
    }

    void on_read(pompeii::Server& s, pompeii::Client& c, const char* buffer, int bytes_read);
};

struct MyServer : public pompeii::ServerEventHandler {
    pompeii::EventLoop& loop;

    MyServer(pompeii::EventLoop& l) : loop(l) {
    }

    void on_client_connect(pompeii::Server& s, pompeii::Client& c) {
        printf("Client connected. Socket: %d\n", c.fd);

        auto mc = std::make_shared<MyClient>();

        c.handler = mc;

        mc->prompt(c);
    }
};

void MyClient::on_read(pompeii::Server& s, pompeii::Client& c, const char* buffer, int bytes_read) {
        std::string_view cmd(buffer, bytes_read);

        std::cout << cmd;

        if (cmd.starts_with("shutdown")) {
            auto h = s.get_handler<MyServer>();
            
           h->loop.end();
        } else if (cmd.starts_with("stats")) {
            display_stats(s);
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