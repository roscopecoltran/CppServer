/*!
    \file udp_client.cpp
    \brief UDP client implementation
    \author Ivan Shynkarenka
    \date 23.12.2016
    \copyright MIT License
*/

#include "server/asio/udp_client.h"

namespace CppServer {
namespace Asio {

UDPClient::UDPClient(std::shared_ptr<Service> service, const std::string& address, int port)
    : _id(CppCommon::UUID::Generate()),
      _service(service),
      _endpoint(asio::ip::udp::endpoint(asio::ip::address::from_string(address), port)),
      _socket(_service->service()),
      _connected(false),
      _datagrams_sent(0),
      _datagrams_received(0),
      _bytes_sent(0),
      _bytes_received(0),
      _reciving(false),
      _sending(false),
      _multicast(false),
      _reuse_address(false)
{
}

UDPClient::UDPClient(std::shared_ptr<Service> service, const asio::ip::udp::endpoint& endpoint)
    : _id(CppCommon::UUID::Generate()),
      _service(service),
      _endpoint(endpoint),
      _socket(_service->service()),
      _connected(false),
      _datagrams_sent(0),
      _datagrams_received(0),
      _bytes_sent(0),
      _bytes_received(0),
      _reciving(false),
      _sending(false),
      _multicast(false),
      _reuse_address(false)
{
}

UDPClient::UDPClient(std::shared_ptr<Service> service, const std::string& address, int port, bool reuse_address)
    : _id(CppCommon::UUID::Generate()),
      _service(service),
      _endpoint(asio::ip::udp::endpoint(asio::ip::address::from_string(address), port)),
      _socket(_service->service()),
      _connected(false),
      _datagrams_sent(0),
      _datagrams_received(0),
      _bytes_sent(0),
      _bytes_received(0),
      _reciving(false),
      _sending(false),
      _multicast(true),
      _reuse_address(reuse_address)
{
}

UDPClient::UDPClient(std::shared_ptr<Service> service, const asio::ip::udp::endpoint& endpoint, bool reuse_address)
    : _id(CppCommon::UUID::Generate()),
      _service(service),
      _endpoint(endpoint),
      _socket(_service->service()),
      _connected(false),
      _datagrams_sent(0),
      _datagrams_received(0),
      _bytes_sent(0),
      _bytes_received(0),
      _reciving(false),
      _sending(false),
      _multicast(true),
      _reuse_address(reuse_address)
{
}

bool UDPClient::Connect()
{
    if (IsConnected())
        return false;

    // Post the connect routine
    auto self(this->shared_from_this());
    _service->service().post([this, self]()
    {
        // Open the client socket
        if (_multicast)
        {
            _socket.open(_endpoint.protocol()),
            _socket.set_option(asio::ip::udp::socket::reuse_address(_reuse_address));
            _socket.bind(_endpoint);
        }
        else
        {
            _socket.open(_endpoint.protocol());
            _socket.bind(asio::ip::udp::endpoint(_endpoint.protocol(), 0));
        }

        // Reset statistic
        _datagrams_sent = 0;
        _datagrams_received = 0;
        _bytes_sent = 0;
        _bytes_received = 0;

        // Update the connected flag
        _connected = true;

        // Call the client connected handler
        onConnected();

        // Try to receive something from the server
        TryReceive();
    });

    return true;
}

bool UDPClient::Disconnect(bool dispatch)
{
    if (!IsConnected())
        return false;

    // Post the disconnect routine
    auto self(this->shared_from_this());
    auto disconnect = [this, self]()
    {
        // Close the client socket
        _socket.close();

        // Clear receive/send buffers
        ClearBuffers();

        // Update the connected flag
        _connected = false;

        // Call the client disconnected handler
        onDisconnected();
    };

    // Dispatch or post the disconnect routine
    if (dispatch)
        _service->Dispatch(disconnect);
    else
        _service->Post(disconnect);

    return true;
}

bool UDPClient::Reconnect()
{
    if (!Disconnect())
        return false;

    while (IsConnected())
        CppCommon::Thread::Yield();

    return Connect();
}

void UDPClient::JoinMulticastGroup(const std::string& address)
{
    if (!IsConnected())
        return;

    asio::ip::address muticast_address = asio::ip::address::from_string(address);

    // Dispatch the join multicast group routine
    auto self(this->shared_from_this());
    service()->Dispatch([this, self, muticast_address]()
    {
        asio::ip::multicast::join_group join(muticast_address);
        _socket.set_option(join);
    });
}

void UDPClient::LeaveMulticastGroup(const std::string& address)
{
    if (!IsConnected())
        return;

    asio::ip::address muticast_address = asio::ip::address::from_string(address);

    // Dispatch the leave multicast group routine
    auto self(this->shared_from_this());
    service()->Dispatch([this, self, muticast_address]()
    {
        asio::ip::multicast::leave_group leave(muticast_address);
        _socket.set_option(leave);
    });
}

size_t UDPClient::Send(const void* buffer, size_t size)
{
    // Send the datagram to the server endpoint
    return Send(_endpoint, buffer, size);
}

size_t UDPClient::Send(const asio::ip::udp::endpoint& endpoint, const void* buffer, size_t size)
{
    assert((buffer != nullptr) && "Pointer to the buffer should not be equal to 'nullptr'!");
    assert((size > 0) && "Buffer size should be greater than zero!");
    if ((buffer == nullptr) || (size == 0))
        return 0;

    if (!IsConnected())
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

void UDPClient::TryReceive()
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

        // Check for disconnect
        if (!IsConnected())
            return;

        // Received datagram from the server
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
        {
            onError(ec.value(), ec.category().name(), ec.message());
            Disconnect(true);
        }
    });
}

void UDPClient::TrySend(const asio::ip::udp::endpoint& endpoint, size_t size)
{
    if (_sending)
        return;

    _sending = true;
    auto self(this->shared_from_this());
    _socket.async_send_to(asio::const_buffer(_send_buffer.data(), _send_buffer.size()), endpoint, [this, self, endpoint](std::error_code ec, size_t sent)
    {
        _sending = false;

        // Check for disconnect
        if (!IsConnected())
            return;

        // Sent datagram to the server
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
        {
            onError(ec.value(), ec.category().name(), ec.message());
            Disconnect(true);
        }
    });
}

void UDPClient::ClearBuffers()
{
    std::lock_guard<std::mutex> locker(_send_lock);
    _recive_buffer.clear();
    _send_buffer.clear();
}

} // namespace Asio
} // namespace CppServer
