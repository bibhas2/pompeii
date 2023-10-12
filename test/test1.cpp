#include <pompeii.h>

struct MyServer : public pompeii::ServerEventHandler {
    void on_client_connect(pompeii::Server& s, pompeii::Client& c) {
        printf("Client connected. Socket: %d\n", c.fd);
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