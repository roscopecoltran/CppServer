/*!
    \file udp_server.cpp
    \brief UDP server implementation
    \author Ivan Shynkarenka
    \date 22.12.2016
    \copyright MIT License
*/

#include "server/asio/udp_server.h"

namespace CppServer {
namespace Asio {

UDPServer::UDPServer(std::shared_ptr<Service> service, InternetProtocol protocol, int port)
    : _service(service),
      _socket(_service->service()),
      _started(false),
      _reciving(false),
      _sending(false),
      _datagrams_sent(0),
      _datagrams_received(0),
      _bytes_sent(0),
      _bytes_received(0)
{
    switch (protocol)
    {
        case InternetProtocol::IPv4:
            _endpoint = asio::ip::udp::endpoint(asio::ip::udp::v4(), port);
            break;
        case InternetProtocol::IPv6:
            _endpoint = asio::ip::udp::endpoint(asio::ip::udp::v6(), port);
            break;
    }
}

UDPServer::UDPServer(std::shared_ptr<Service> service, const std::string& address, int port)
    : _service(service),
      _socket(_service->service()),
      _started(false),
      _datagrams_sent(0),
      _datagrams_received(0),
      _bytes_sent(0),
      _bytes_received(0)
{
    _endpoint = asio::ip::udp::endpoint(asio::ip::address::from_string(address), port);
}

UDPServer::UDPServer(std::shared_ptr<Service> service, const asio::ip::udp::endpoint& endpoint)
    : _service(service),
      _endpoint(endpoint),
      _socket(_service->service()),
      _started(false),
      _datagrams_sent(0),
      _datagrams_received(0),
      _bytes_sent(0),
      _bytes_received(0)
{
}

bool UDPServer::Start()
{
    assert(!IsStarted() && "UDP server is already started!");
    if (IsStarted())
        return false;

    // Post the start routine
    auto self(this->shared_from_this());
    _service->service().post([this, self]()
    {
        // Open the server socket
        _socket = asio::ip::udp::socket(_service->service(), _endpoint);

        // Reset statistic
        _datagrams_sent = 0;
        _datagrams_received = 0;
        _bytes_sent = 0;
        _bytes_received = 0;

         // Update the started flag
        _started = true;

        // Call the server started handler
        onStarted();

        // Try to receive datagrams from the clients
        TryReceive();
    });

    return true;
}

bool UDPServer::Start(const std::string& multicast_address, int multicast_port)
{
    _multicast_endpoint = asio::ip::udp::endpoint(asio::ip::address::from_string(multicast_address), multicast_port);
    return Start();
}

bool UDPServer::Start(const asio::ip::udp::endpoint& multicast_endpoint)
{
    _multicast_endpoint = multicast_endpoint;
    return Start();
}

bool UDPServer::Stop()
{
    assert(IsStarted() && "UDP server is not started!");
    if (!IsStarted())
        return false;

    // Post the stopped routine
    auto self(this->shared_from_this());
    _service->service().post([this, self]()
    {
        // Close the server socket
        _socket.close();

        // Clear receive/send buffers
        ClearBuffers();

        // Update the started flag
        _started = false;

        // Call the server stopped handler
        onStopped();
    });

    return true;
}

bool UDPServer::Restart()
{
    if (!Stop())
        return false;

    while (IsStarted())
        CppCommon::Thread::Yield();

    return Start();
}

size_t UDPServer::Multicast(const void* buffer, size_t size)
{
    // Send the datagram to the multicast endpoint
    return Send(_multicast_endpoint, buffer, size);
}

size_t UDPServer::Send(const asio::ip::udp::endpoint& endpoint, const void* buffer, size_t size)
{
    assert((buffer != nullptr) && "Pointer to the buffer should not be equal to 'nullptr'!");
    assert((size > 0) && "Buffer size should be greater than zero!");
    if ((buffer == nullptr) || (size == 0))
        return 0;

    if (!IsStarted())
        return 0;

    // Fill the send buffer
    {
        std::lock_guard<std::mutex> locker(_send_lock);
        const uint8_t* bytes = (const uint8_t*)buffer;
        _send_buffer.insert(_send_buffer.end(), bytes, bytes + size);
    }

    // Dispatch the send routine
    auto self(this->shared_from_this());
    service()->Dispatch([this, self, endpoint, size]()
    {
        // Try to send the datagram
        TrySend(endpoint, size);
    });

    return _send_buffer.size();
}

void UDPServer::TryReceive()
{
    if (_reciving)
        return;

    // Prepare receive buffer
    size_t old_size = _recive_buffer.size();
    _recive_buffer.resize(_recive_buffer.size() + CHUNK);

    _reciving = true;
    auto self(this->shared_from_this());
    _socket.async_receive_from(asio::buffer(_recive_buffer.data(), _recive_buffer.size()), _recive_endpoint, [this, self](std::error_code ec, size_t received)
    {
        _reciving = false;

        // Check if the server is stopped
        if (!IsStarted())
            return;

        // Received datagram from the client
        if (received > 0)
        {
            // Update statistic
            ++_datagrams_received;
            _bytes_received += received;

            // Prepare receive buffer
            _recive_buffer.resize(_recive_buffer.size() - (CHUNK - received));

            // Call the datagram received handler
            onReceived(_recive_endpoint, _recive_buffer.data(), _recive_buffer.size());

            // Clear the handled buffer
            _recive_buffer.clear();
        }

        // Try to receive again if the session is valid
        if (!ec || (ec == asio::error::would_block))
            TryReceive();
        else
            onError(ec.value(), ec.category().name(), ec.message());
    });
}

void UDPServer::TrySend(const asio::ip::udp::endpoint& endpoint, size_t size)
{
    if (_sending)
        return;

    _sending = true;
    auto self(this->shared_from_this());
    _socket.async_send_to(asio::const_buffer(_send_buffer.data(), _send_buffer.size()), endpoint, [this, self, endpoint](std::error_code ec, size_t sent)
    {
        _sending = false;

        // Check if the server is stopped
        if (!IsStarted())
            return;

        // Sent datagram to the client
        if (sent > 0)
        {
            // Update statistic
            ++_datagrams_sent;
            _bytes_sent += sent;

            // Erase the sent buffer
            {
                std::lock_guard<std::mutex> locker(_send_lock);
                _send_buffer.erase(_send_buffer.begin(), _send_buffer.begin() + sent);
            }

            // Call the datagram sent handler
            onSent(endpoint, sent, 0);

            // Stop sending
            return;
        }

        // Try to send again if the session is valid
        if (!ec || (ec == asio::error::would_block))
            return;
        else
            onError(ec.value(), ec.category().name(), ec.message());
    });
}

void UDPServer::ClearBuffers()
{
    std::lock_guard<std::mutex> locker(_send_lock);
    _recive_buffer.clear();
    _send_buffer.clear();
}

} // namespace Asio
} // namespace CppServer
