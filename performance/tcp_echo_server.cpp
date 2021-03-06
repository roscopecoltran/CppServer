#include "server/asio/service.h"
#include "server/asio/tcp_server.h"

#include <atomic>
#include <iostream>

#include "../../modules/cpp-optparse/OptionParser.h"

class EchoSession;

class EchoServer : public CppServer::Asio::TCPServer<EchoServer, EchoSession>
{
public:
    using CppServer::Asio::TCPServer<EchoServer, EchoSession>::TCPServer;
};

class EchoSession : public CppServer::Asio::TCPSession<EchoServer, EchoSession>
{
public:
    using CppServer::Asio::TCPSession<EchoServer, EchoSession>::TCPSession;

protected:
    size_t onReceived(const void* buffer, size_t size) override
    {
        // Resend the message back to the client
        Send(buffer, size);

        // Inform that we handled the whole buffer
        return size;
    }
};

int main(int argc, char** argv)
{
    auto parser = optparse::OptionParser().version("1.0.0.0");

    parser.add_option("-h", "--help").help("Show help");
    parser.add_option("-p", "--port").action("store").type("int").set_default(1111).help("Server port. Default: %default");

    optparse::Values options = parser.parse_args(argc, argv);

    // Print help
    if (options.get("help"))
    {
        parser.print_help();
        parser.exit();
    }

    // Server port
    int port = options.get("port");

    std::cout << "Server port: " << port << std::endl;

    // Create a new Asio service
    auto service = std::make_shared<CppServer::Asio::Service>();

    // Start the service
    std::cout << "Asio service starting...";
    service->Start();
    std::cout << "Done!" << std::endl;

    // Create a new echo server
    auto server = std::make_shared<EchoServer>(service, CppServer::Asio::InternetProtocol::IPv4, port);

    // Start the server
    std::cout << "Server starting...";
    server->Start();
    std::cout << "Done!" << std::endl;

    std::cout << "Press Enter to stop the server or '!' to restart the server..." << std::endl;

    // Perform text input
    std::string line;
    while (getline(std::cin, line))
    {
        if (line.empty())
            break;

        // Restart the server
        if (line == "!")
        {
            std::cout << "Server restarting...";
            server->Restart();
            std::cout << "Done!" << std::endl;
            continue;
        }
    }

    // Stop the server
    std::cout << "Server stopping...";
    server->Stop();
    std::cout << "Done!" << std::endl;

    // Stop the service
    std::cout << "Asio service stopping...";
    service->Stop();
    std::cout << "Done!" << std::endl;

    return 0;
}
