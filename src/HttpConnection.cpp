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

#include "HttpConnection.h"

#include "Basics/cpu-relax.h"

#include <velocypack/Parser.h>
#include <atomic>
#include <cassert>

#include <fuerte/helper.h>
#include <fuerte/loop.h>
#include <fuerte/types.h>

namespace {
using namespace arangodb::fuerte::v1;
using namespace arangodb::fuerte::v1::http;

int on_message_began(http_parser* parser) { return 0; }
int on_status(http_parser* parser, const char* at, size_t len) { return 0; }
int on_header_field(http_parser* parser, const char* at, size_t len) {
  RequestItem* data = static_cast<RequestItem*>(parser->data);
  if (data->last_header_was_a_value) {
    data->_response->header.meta.emplace(std::move(data->lastHeaderField),
                                         std::move(data->lastHeaderValue));
    data->lastHeaderField.assign(at, len);
  } else {
    data->lastHeaderField.append(at, len);
  }
  data->last_header_was_a_value = false;
  return 0;
}
static int on_header_value(http_parser* parser, const char* at, size_t len) {
  RequestItem* data = static_cast<RequestItem*>(parser->data);
  if (data->last_header_was_a_value) {
    data->lastHeaderValue.append(at, len);
  } else {
    data->lastHeaderValue.assign(at, len);
  }
  data->last_header_was_a_value = true;
  return 0;
}
static int on_header_complete(http_parser* parser) {
  RequestItem* data = static_cast<RequestItem*>(parser->data);
  data->_response->header.responseCode =
      static_cast<StatusCode>(parser->status_code);
  if (!data->lastHeaderField.empty()) {
    data->_response->header.meta.emplace(std::move(data->lastHeaderField),
                                         std::move(data->lastHeaderValue));
  }
  return 0;
}
static int on_body(http_parser* parser, const char* at, size_t len) {
  static_cast<RequestItem*>(parser->data)->_responseBuffer.append(at, len);
  return 0;
}
static int on_message_complete(http_parser* parser) {
  static_cast<RequestItem*>(parser->data)->message_complete = true;
  return 0;
}
}  // namespace

namespace arangodb { namespace fuerte { inline namespace v1 { namespace http {

namespace fu = ::arangodb::fuerte::v1;
using namespace arangodb::fuerte::detail;

HttpConnection::HttpConnection(std::shared_ptr<asio_io_context>& ctx,
                               ConnectionConfiguration const& configuration)
    : AsioConnection(ctx, configuration), _inFlight(nullptr) {
  _parserSettings.on_message_begin = ::on_message_began;
  _parserSettings.on_status = ::on_status;
  _parserSettings.on_header_field = ::on_header_field;
  _parserSettings.on_header_value = ::on_header_value;
  _parserSettings.on_headers_complete = ::on_header_complete;
  _parserSettings.on_body = ::on_body;
  _parserSettings.on_message_complete = ::on_message_complete;
  http_parser_init(&_parser, HTTP_RESPONSE);
}

MessageID HttpConnection::sendRequest(std::unique_ptr<Request> request,
                                      RequestCallback callback) {
  FUERTE_LOG_HTTPTRACE << "queueRequest - start - at address: " << request.get()
                       << std::endl;
  static std::atomic<uint64_t> ticketId(1);

  // Prepare a new request
  uint64_t id = ticketId.fetch_add(1, std::memory_order_relaxed);
  request->messageID = id;
  auto item = createRequestItem(std::move(request), callback);

  uint32_t state = queueRequest(std::move(item));
  if (_connected) {
    FUERTE_LOG_HTTPTRACE << "sendRequest (http): start sending & reading"
                         << std::endl;
    // HTTP is half-duplex protocol: we only write if there is no reading
    if (!(state & LOOP_FLAGS)) {
      startWriting();
    }
  } else {
    FUERTE_LOG_HTTPTRACE << "sendRequest (http): not connected" << std::endl;
  }
  return id;
}

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

std::unique_ptr<RequestItem> HttpConnection::createRequestItem(
    std::unique_ptr<Request> request, RequestCallback callback) {
  assert(request);
  // build the request header
  assert(request->header.restVerb != RestVerb::Illegal);

  std::string header;
  header.reserve(128);  // TODO is there a meaningful size ?
  header.append(fu::to_string(request->header.restVerb));
  header.push_back(' ');

  // construct request path ("/_db/<name>/" prefix)
  if (!request->header.database.empty()) {
    header.append("/_db/");
    header.append(http::urlEncode(request->header.database));
  }

  // TODO these should throw an exception somewhere else
  assert(!request->header.path.empty());
  assert(request->header.path[0] == '/');

  auto const& parameters = request->header.parameters;
  if (parameters.empty()) {
    header.append(request->header.path);
  } else {
    std::string path = request->header.path;
    path.push_back('?');
    for (auto p : parameters) {
      if (path.back() != '?') {
        path.push_back('&');
      }
      path += http::urlEncode(p.first) + "=" + http::urlEncode(p.second);
    }
  }
  header.append(" HTTP/1.1\r\n");
  header.append("Host: ");
  header.append(_configuration._host);
  header.append("\r\n");
  // TODO add option to configuration
  header.append("Connection: Keep-Alive\r\n");
  // header.append("Connection: Close\r\n");
  for (auto const& pair : request->header.meta) {
    header.append(pair.first);
    header.append(": ");
    header.append(pair.second);
    header.append("\r\n");
  }

  if (_configuration._authenticationType == AuthenticationType::Basic) {
    header.append("Authorization: Basic ");
    header.append(fu::encodeBase64(_configuration._user + ":" +
                                   _configuration._password));
    header.append("\r\n");
  } else if (_configuration._authenticationType == AuthenticationType::Jwt) {
    assert(false);
    /*header.append("Authorization: bearer ");
    header.append(_configuration._);
    header.append("\r\n");*/
  }

  if (request->header.restVerb != RestVerb::Get) {
    header.append("Content-Length: ");
    header.append(std::to_string(request->payloadSize()));
    header.append("\r\n\r\n");
  } else {
    header.append("\r\n");
  }
  // body will be appended seperately

  // construct RequestItem
  std::unique_ptr<RequestItem> requestItem(
      new RequestItem());  // std::make_shared<RequestItem>(, callback);
  requestItem->_request = std::move(request);
  requestItem->_callback = std::move(callback);
  // requestItem->_response later
  requestItem->_requestHeader = std::move(header);
  return requestItem;
}

// socket connection is up (with optional SSL)
void HttpConnection::finishInitialization() {
  _connected = true;
  startWriting();  // starts writing queue if non-empty
}

// called on shutdown, always call superclass
void HttpConnection::shutdownConnection(const ErrorCondition ec) {
  AsioConnection::shutdownConnection(ec);
  // simon: thread-safe, only called from IO-Thread (which holds shared_ptr) and
  // destructor
  if (_inFlight) {
    // Item has failed, remove from message store
    _messageStore.removeByID(_inFlight->_request->messageID);
    _inFlight->invokeOnError(errorToInt(ec), std::move(_inFlight->_request),
                             {nullptr});
    _inFlight.reset();
  }
}

// fetch the buffers for the write-loop (called from IO thread)
std::vector<boost::asio::const_buffer> HttpConnection::fetchBuffers(
    std::shared_ptr<RequestItem> const& item) {
  _messageStore.add(item);
  return {boost::asio::buffer(item->_requestHeader.data(),
                              item->_requestHeader.size()),
          item->_request->payload()};
}

// Thread-Safe: activate the combined write-read loop
void HttpConnection::startWriting() {
  assert(_connected);
  FUERTE_LOG_HTTPTRACE << "startWriting (http): this=" << this << std::endl;

  // we want to turn on both flags at once
  uint32_t state = _loopState.load(std::memory_order_seq_cst);
  while (!(state & LOOP_FLAGS) && (state & WRITE_LOOP_QUEUE_MASK) > 0) {
    if (_loopState.compare_exchange_weak(state, state | LOOP_FLAGS,
                                         std::memory_order_seq_cst)) {
      FUERTE_LOG_HTTPTRACE << "startWriting (http: starting write\n";
      asyncWrite();  // only one thread can get here
    }
    cpu_relax();
  }
  if ((state & WRITE_LOOP_QUEUE_MASK) == 0) {
    FUERTE_LOG_HTTPTRACE << "startWriting (http: nothing is queued\n";
  }
}

// called by the async_write handler (called from IO thread)
void HttpConnection::asyncWriteCallback(
    ::boost::system::error_code const& error, size_t transferred,
    std::shared_ptr<RequestItem> item) {
  // Cancel deadline
  //_deadline.cancel();

  if (error) {
    // Send failed
    FUERTE_LOG_CALLBACKS << "asyncWriteCallback (http): error "
                         << error.message() << std::endl;
    FUERTE_LOG_ERROR << error.message() << std::endl;

    // Item has failed, remove from message store
    _messageStore.removeByID(item->_request->messageID);

    // let user know that this request caused the error
    item->_callback.invoke(errorToInt(ErrorCondition::WriteError),
                           std::move(item->_request), nullptr);

    // Stop current connection and try to restart a new one.
    // This will reset the current write loop.
    restartConnection(ErrorCondition::WriteError);

  } else {
    // Send succeeded
    FUERTE_LOG_CALLBACKS << "asyncWriteCallback (http): send succeeded, "
                         << transferred << " bytes transferred\n";
    // async-calls=" << pendingAsyncCalls << std::endl;

    // request is written we no longer need data for that
    item->_requestHeader.clear();

    // thead-safe we are on the single IO-Thread
    assert(_inFlight == nullptr);
    _inFlight = std::move(item);
    assert(_inFlight->_response == nullptr);
    _inFlight->_response.reset(new Response());
    _inFlight->_response->messageID = _inFlight->_request->messageID;

    http_parser_init(&_parser, HTTP_RESPONSE);
    _parser.data = static_cast<void*>(_inFlight.get());

    // check queue length later
    asyncReadSome();  // listen for the response

    FUERTE_LOG_HTTPTRACE << "asyncWriteCallback (http): waiting for response"
                         << std::endl;
  }
}

// called by the async_read handler (called from IO thread)
void HttpConnection::asyncReadCallback(::boost::system::error_code const& ec,
                                       size_t transferred) {
  if (ec) {
    FUERTE_LOG_CALLBACKS
        << "asyncReadCallback: Error while reading from socket";
    FUERTE_LOG_ERROR << ec.message() << std::endl;

    // Restart connection, this will trigger a release of the readloop.
    restartConnection(
        ErrorCondition::ConnectionError);  // will invoke _inFlight cb

  } else {
    FUERTE_LOG_CALLBACKS
        << "asyncReadCallback: received " << transferred
        << " bytes\n";  // async-calls=" << pendingAsyncCalls << std::endl;

    // Inspect the data we've received so far.
    /*auto buffers = _receiveBuffer.data(); // no copy
     for (auto const& buffer : buffers) {
     buffer.data()
     }*/

    // Inspect the data we've received so far.
    auto recvBuffs = _receiveBuffer.data();  // no copy
    auto cursor = boost::asio::buffer_cast<const char*>(
        recvBuffs);  // TODO this is deprecated
    auto available = boost::asio::buffer_size(recvBuffs);

    assert(_inFlight);  // must be set earlier
    /* Start up / continue the parser.
     * Note we pass recved==0 to signal that EOF has been received.
     */
    size_t nparsed =
        http_parser_execute(&_parser, &_parserSettings, cursor, available);

    // Remove consumed data from receive buffer.
    _receiveBuffer.consume(nparsed);

    if (_parser.upgrade) {
      /* handle new protocol */
      FUERTE_LOG_ERROR << "Upgrading is not supported" << std::endl;
      shutdownConnection(
          ErrorCondition::ProtocolError);  // will cleanup _inFlight
      return;
    } else if (nparsed != transferred) {
      /* Handle error. Usually just close the connection. */
      FUERTE_LOG_ERROR << "Invalid HTTP response in parser" << std::endl;
      shutdownConnection(
          ErrorCondition::ProtocolError);  // will cleanup _inFlight
      return;
    } else if (_inFlight->message_complete) {
      // remove processed item from the message store
      _messageStore.removeByID(_inFlight->_request->messageID);
      // thread-safe access on IO-Thread
      _inFlight->_response->setPayload(std::move(_inFlight->_responseBuffer),
                                       0);
      _inFlight->_callback.invoke(0, std::move(_inFlight->_request),
                                  std::move(_inFlight->_response));
      _inFlight.reset();

      FUERTE_LOG_HTTPTRACE
          << "asyncReadCallback (http): completed parsing response\n";

      // check the queue length, stop IO loop if empty
      uint32_t state = _loopState.load(std::memory_order_seq_cst);
      // nothing is queued, lets try to halt the write queue while
      // the write loop is active and nothing is queued
      while ((state & LOOP_FLAGS) && (state & WRITE_LOOP_QUEUE_MASK) == 0) {
        if (_loopState.compare_exchange_weak(state, state & ~LOOP_FLAGS)) {
          FUERTE_LOG_TRACE << "asyncWrite: no more queued items" << std::endl;
          return;  // we turned loop off while nothing was queued
        }
        cpu_relax();
      }

      assert(state & LOOP_FLAGS);
      assert((state & WRITE_LOOP_QUEUE_MASK) > 0);
      asyncWrite();  // send next request, do not keep reading

    } else {
      FUERTE_LOG_HTTPTRACE
          << "asyncReadCallback (http): response not complete yet\n";
      asyncReadSome();  // keep reading from socket
    }
  }
}
}}}}  // namespace arangodb::fuerte::v1::http
