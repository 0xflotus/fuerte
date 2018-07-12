////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "AsioConnection.h"

#include <fuerte/FuerteLogger.h>
#include <boost/asio/connect.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include "Basics/cpu-relax.h"
#include "http.h"
#include "vst.h"

namespace arangodb { namespace fuerte { inline namespace v1 {
using bt = ::boost::asio::ip::tcp;
using be = ::boost::asio::ip::tcp::endpoint;
using BoostEC = ::boost::system::error_code;

template <typename T>
AsioConnection<T>::AsioConnection(
    std::shared_ptr<boost::asio::io_context> const& ctx,
    detail::ConnectionConfiguration const& config)
    : Connection(config),
      _io_context(ctx),
      _resolver(*ctx),
      _socket(nullptr),
      _timeout(*ctx),
      _sslContext(nullptr),
      _sslSocket(nullptr),
      _connected(false),
      _permanent_failure(false),
      _loopState(0),
      _writeQueue(1024) {}

// Deconstruct.
template <typename T>
AsioConnection<T>::~AsioConnection() {
  _resolver.cancel();
  if (!_messageStore.empty()) {
    FUERTE_LOG_HTTPTRACE << "DESTROYING CONNECTION WITH: "
                         << _messageStore.size() << " outstanding requests!"
                         << std::endl;
  }
  shutdownConnection();
}

// Activate this connection.
template <typename T>
void AsioConnection<T>::start() {
  startResolveHost();
}

// resolve the host into a series of endpoints
template <typename T>
void AsioConnection<T>::startResolveHost() {
  // Resolve the host asynchronous.
  auto self = shared_from_this();
  _resolver.async_resolve(
      {_configuration._host, _configuration._port},
      [this, self](const boost::system::error_code& error,
                   bt::resolver::iterator iterator) {
        if (error) {
          FUERTE_LOG_ERROR << "resolve failed: error=" << error << std::endl;
          onFailure(errorToInt(ErrorCondition::CouldNotConnect),
                    "resolved failed: error" + error.message());
        } else {
          FUERTE_LOG_CALLBACKS << "resolve succeeded" << std::endl;
          _endpoints = iterator;
          if (_endpoints == bt::resolver::iterator()) {
            FUERTE_LOG_ERROR << "unable to resolve endpoints" << std::endl;
            onFailure(errorToInt(ErrorCondition::CouldNotConnect),
                      "unable to resolve endpoints");
          } else {
            initSocket();
          }
        }
      });
}

// CONNECT RECONNECT //////////////////////////////////////////////////////////

template <typename T>
void AsioConnection<T>::initSocket() {
  // std::lock_guard<std::mutex> lock(_socket_mutex);

  // socket must be empty before. Check that
  assert(!_socket);
  assert(!_sslSocket);

  FUERTE_LOG_CALLBACKS << "begin init" << std::endl;
  _socket.reset(new bt::socket(*_io_context));
  if (_configuration._ssl) {
    _sslContext.reset(new boost::asio::ssl::context(
        boost::asio::ssl::context::method::sslv23));
    _sslContext->set_default_verify_paths();
    _sslSocket.reset(
        new boost::asio::ssl::stream<bt::socket&>(*_socket, *_sslContext));
  }

  auto endpoints = _endpoints;  // copy as connect modifies iterator
  startConnect(endpoints);
}

// close the TCP & SSL socket.
template <typename T>
void AsioConnection<T>::shutdownSocket() {
  std::lock_guard<std::mutex> lock(_socket_mutex);

  FUERTE_LOG_CALLBACKS << "begin shutdown socket\n";

  ::boost::system::error_code error;
  if (_sslSocket) {
    _sslSocket->shutdown(error);
  }
  if (_socket) {
    _socket->cancel();
    _socket->shutdown(bt::socket::shutdown_both, error);
    _socket->close(error);
  }
  _sslSocket = nullptr;
  _socket = nullptr;
}

// shutdown the connection and cancel all pending messages.
template <typename T>
void AsioConnection<T>::shutdownConnection(const ErrorCondition ec) {
  FUERTE_LOG_CALLBACKS << "shutdownConnection\n";
  _connected = false;

  // Stop the read & write loop
  stopIOLoops();

  // Close socket
  shutdownSocket();

  // Cancel all items and remove them from the message store.
  _messageStore.cancelAll(ec);
}

template <typename T>
void AsioConnection<T>::restartConnection(const ErrorCondition error) {
  // Read & write loop must have been reset by now

  FUERTE_LOG_CALLBACKS << "restartConnection" << std::endl;
  // Terminate connection
  shutdownConnection(error);

  // Initiate new connection
  if (!_permanent_failure) {
    startResolveHost();
  }
}

// Thread-Safe: reset io loop flags
template <typename T>
void AsioConnection<T>::stopIOLoops() {
  uint32_t state = _loopState.load(std::memory_order_seq_cst);
  while (state & LOOP_FLAGS) {
    if (_loopState.compare_exchange_weak(state, state & ~LOOP_FLAGS,
                                         std::memory_order_seq_cst)) {
      FUERTE_LOG_TRACE << "stopIOLoops: loops stopped" << std::endl;
      return;  // we turned flag off while nothin was queued
    }
    cpu_relax();
  }
}

// ------------------------------------
// Creating a connection
// ------------------------------------

// try to open the socket connection to the first endpoint.
template <typename T>
void AsioConnection<T>::startConnect(bt::resolver::iterator endpointItr) {
  if (endpointItr != boost::asio::ip::tcp::resolver::iterator()) {
    FUERTE_LOG_CALLBACKS << "trying to connect to: " << endpointItr->endpoint()
                         << "..." << std::endl;

    // Set a deadline for the connect operation.
    //_deadline.expires_from_now(boost::posix_time::seconds(60));
    // TODO wait for connect timeout

    // Start the asynchronous connect operation.
    auto self = shared_from_this();
    boost::asio::async_connect(
        *_socket, endpointItr,
        std::bind(&AsioConnection<T>::asyncConnectCallback,
                  std::static_pointer_cast<AsioConnection>(self),
                  std::placeholders::_1, endpointItr));
  }
}

// callback handler for async_callback (called in startConnect).
template <typename T>
void AsioConnection<T>::asyncConnectCallback(
    BoostEC const& error, bt::resolver::iterator endpointItr) {
  if (error) {
    // Connection failed
    FUERTE_LOG_ERROR << error.message() << std::endl;
    shutdownConnection();
    if (endpointItr == bt::resolver::iterator()) {
      FUERTE_LOG_CALLBACKS << "no further endpoint" << std::endl;
    }
    onFailure(errorToInt(ErrorCondition::CouldNotConnect),
              "unable to connect -- " + error.message());
    _messageStore.cancelAll(ErrorCondition::CouldNotConnect);
  } else {
    // Connection established
    FUERTE_LOG_CALLBACKS << "TCP socket connected" << std::endl;
    if (_configuration._ssl) {
      startSSLHandshake();
    } else {
      finishInitialization();
    }
  }
}

// start intiating an SSL connection (on top of an established TCP socket)
template <typename T>
void AsioConnection<T>::startSSLHandshake() {
  if (!_configuration._ssl) {
    finishInitialization();
  }
  // https://www.boost.org/doc/libs/1_67_0/doc/html/boost_asio/overview/ssl.html
  // Perform SSL handshake and verify the remote host's certificate.
  _sslSocket->set_verify_mode(boost::asio::ssl::verify_peer);
  _sslSocket->set_verify_callback(
      boost::asio::ssl::rfc2818_verification(_configuration._host));

  FUERTE_LOG_CALLBACKS << "starting ssl handshake " << std::endl;
  auto self = shared_from_this();
  _sslSocket->async_handshake(
      boost::asio::ssl::stream_base::client,
      [this, self](BoostEC const& error) {
        if (error) {
          FUERTE_LOG_ERROR << error.message() << std::endl;
          shutdownSocket();
          onFailure(
              errorToInt(ErrorCondition::CouldNotConnect),
              "unable to perform ssl handshake: error=" + error.message());
          _messageStore.cancelAll(ErrorCondition::CouldNotConnect);
        } else {
          FUERTE_LOG_CALLBACKS << "ssl handshake done" << std::endl;
          finishInitialization();
        }
      });
}

// ------------------------------------
// Writing data
// ------------------------------------

// Thread-Safe: queue a new request
template <typename T>
uint32_t AsioConnection<T>::queueRequest(std::unique_ptr<T> item) {
  if (!_writeQueue.push(item.get())) {
    FUERTE_LOG_ERROR << "connection queue capactiy exceeded" << std::endl;
    throw std::length_error("connection queue capactiy exceeded");
  }
  item.release();
  // WRITE_LOOP_ACTIVE, READ_LOOP_ACTIVE are synchronized via cmpxchg
  return _loopState.fetch_add(WRITE_LOOP_QUEUE_INC, std::memory_order_seq_cst);
}

// writes data from task queue to network using boost::asio::async_write
template <typename T>
void AsioConnection<T>::asyncWrite() {
  FUERTE_LOG_TRACE << "asyncWrite: preparing to send next" << std::endl;
  if (_permanent_failure || !_connected) {
    FUERTE_LOG_TRACE << "asyncReadSome: permanent failure\n";
    stopIOLoops();  // will set the flag
    return;
  }

  // reduce queue length and check active flag
  uint32_t state =
      _loopState.fetch_sub(WRITE_LOOP_QUEUE_INC, std::memory_order_acquire);
  assert((state & WRITE_LOOP_QUEUE_MASK) > 0);

  T* ptr;
  assert(_writeQueue.pop(ptr));  // should never fail here
  std::shared_ptr<T> item(ptr);

  // we stop the write-loop if we stopped it ourselves.
  auto self = shared_from_this();
  auto cb = [this, self, item](BoostEC const& ec, std::size_t transferred) {
    asyncWriteCallback(ec, transferred, std::move(item));
  };
  auto buffers = this->fetchBuffers(item);
  if (_configuration._ssl) {
    boost::asio::async_write(*_sslSocket, buffers, cb);
    /*boost::asio::async_write(
        *_sslSocket, buffers,
        std::bind(&AsioConnection<T>::asyncWriteCallback,
                  std::static_pointer_cast<AsioConnection<T>>(self),
                  std::placeholders::_1, std::placeholders::_2,
                  std::move(item)));*/
  } else {
    boost::asio::async_write(*_socket, buffers, cb);
    /*boost::asio::async_write(
        *_socket, buffers,
        std::bind(&AsioConnection<T>::asyncWriteCallback,
                  std::static_pointer_cast<AsioConnection<T>>(self),
                  std::placeholders::_1, std::placeholders::_2,
                  std::move(item)));*/
  }

  FUERTE_LOG_TRACE << "asyncWrite: done" << std::endl;
}

// ------------------------------------
// Reading data
// ------------------------------------

// asyncReadSome reads the next bytes from the server.
template <typename T>
void AsioConnection<T>::asyncReadSome() {
  FUERTE_LOG_TRACE << "asyncReadSome: this=" << this << std::endl;

  if (!(_loopState.load(std::memory_order_seq_cst) & READ_LOOP_ACTIVE)) {
    FUERTE_LOG_TRACE << "asyncReadSome: read-loop was stopped\n";
    return;  // just stop
  }
  if (_permanent_failure || !_connected) {
    FUERTE_LOG_TRACE << "asyncReadSome: permanent failure\n";
    stopIOLoops();  // will set the flags
    return;
  }

  // Start reading data from the network.
  FUERTE_LOG_CALLBACKS << "r";
#if ENABLE_FUERTE_LOG_CALLBACKS > 0
  std::cout << "_messageMap = " << _messageStore.keys() << std::endl;
#endif

  // Set timeout
  /*auto self = shared_from_this();
  _deadline.expires_from_now(boost::posix_time::milliseconds(timeout.count()));
  _deadline.async_wait(boost::bind(&ReadLoop::deadlineHandler, self, _1));

  _connection->_async_calls++;*/

  assert(_socket);
  auto self = shared_from_this();
  auto cb = [this, self](BoostEC ec, size_t transferred) {
    // received data is "committed" from output sequence to input sequence
    _receiveBuffer.commit(transferred);
    asyncReadCallback(ec, transferred);
  };

  // reserve 32kB in output buffer
  auto mutableBuff = _receiveBuffer.prepare(READ_BLOCK_SIZE);
  if (_configuration._ssl) {
    _sslSocket->async_read_some(mutableBuff, cb);
  } else {
    _socket->async_read_some(mutableBuff, cb);
  }

  FUERTE_LOG_TRACE << "asyncReadSome: done" << std::endl;
}

template class arangodb::fuerte::v1::AsioConnection<
    arangodb::fuerte::v1::vst::RequestItem>;
template class arangodb::fuerte::v1::AsioConnection<
    arangodb::fuerte::v1::http::RequestItem>;
}}}  // namespace arangodb::fuerte::v1
