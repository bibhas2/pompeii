#include <pompeii.h>
#include <string_view>
#include <sstream>

struct MyClient : public pompeii::ClientEventHandler {
    char read_buff[256];
    std::ostringstream out;
    std::string write_buffer;

    ~MyClient() {
        printf("MyClient getting cleaned up.\n");
    }
    void prompt(pompeii::Client& c) {
        c.cancel_write();
        c.cancel_read();

        out << "> ";
        
        send_output(c);

        c.schedule_read(read_buff, sizeof(read_buff));
    }

    void display_stats(pompeii::Server& s, pompeii::Client& c) {
        printf("display_stats called\n");
        for (auto& c : s.client_state) {
            if (c.in_use()) {
                out << "Client: " << c.fd << "\n";
                out << "Reading: " << ((c.read_write_flag & pompeii::RW_STATE_READ) ? "Y" : "N") << "\n";
                out << "Writing: " << ((c.read_write_flag & pompeii::RW_STATE_WRITE) ? "Y" : "N") << "\n";
            }
        }

        send_output(c);
    }

    void send_output(pompeii::Client& c) {
        c.cancel_write();

        write_buffer = out.str();

        out.clear();
        out.str("");

        c.schedule_write(write_buffer.data(), write_buffer.length());
    }

    void on_write_completed(pompeii::Server& s, pompeii::Client& c) {
        if (!(c.read_write_flag & pompeii::RW_STATE_READ)) {
            //Not already waiting to read

            prompt(c);
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

        if (cmd.starts_with("shutdown")) {
            auto h = s.get_handler<MyServer>();
            
           h->loop.end();
        } else if (cmd.starts_with("stats")) {
            display_stats(s, c);
        }

        c.cancel_read();
}

int main() {
    pompeii::enable_trace(1);

    pompeii::EventLoop loop;

    loop.add_server(9080, std::make_shared<MyServer>(loop));

    loop.start();
}