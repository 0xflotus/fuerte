////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Christoph Uhde
/// @author Ewout Prangsma
////////////////////////////////////////////////////////////////////////////////
#pragma once
#ifndef ARANGO_CXX_DRIVER_CONNECTION
#define ARANGO_CXX_DRIVER_CONNECTION

#include "types.h"
#include "message.h"
#include "loop.h"

#include <memory>
#include <string>

namespace arangodb { namespace fuerte { inline namespace v1 {

// Connection is the base class for a connection between a client
// and a server.
// Different protocols (HTTP, VST) are implemented in derived classes.
class Connection : public std::enable_shared_from_this<Connection> {
  friend class ConnectionBuilder;

  public:
    virtual ~Connection();
    
    // Send a request to the server and wait into a response it received.
    std::unique_ptr<Response> sendRequest(std::unique_ptr<Request> r);

    // Send a request to the server and wait into a response it received.
    std::unique_ptr<Response> sendRequest(Request const& r){
      std::unique_ptr<Request> copy(new Request(r));
      return sendRequest(std::move(copy));
    }

    // Send a request to the server and return immediately.
    // When a response is received or an error occurs, the corresponding callback is called.
    virtual MessageID sendRequest(std::unique_ptr<Request> r, RequestCallback cb) = 0;

    // Send a request to the server and return immediately.
    // When a response is received or an error occurs, the corresponding callback is called.
    MessageID sendRequest(Request const& r, RequestCallback cb) {
      std::unique_ptr<Request> copy(new Request(r));
      return sendRequest(std::move(copy), cb);
    }

    // Return the number of requests that have not yet finished.
    virtual std::size_t requestsLeft() = 0;

  private:
    // Activate the connection.  
    virtual void start() {}

  protected:
    Connection(EventLoopService& eventLoopService, detail::ConnectionConfiguration const& conf) :
      _eventLoopService(eventLoopService),
      _configuration(conf) {}

    // Invoke the configured ConnectionFailureCallback (if any)
    void onFailure(Error errorCode, const std::string& errorMessage) {
      if (_configuration._onFailure) {
        _configuration._onFailure(errorCode, errorMessage);
      }
    }

    EventLoopService& _eventLoopService;
    const detail::ConnectionConfiguration _configuration;
};

/** The connection Builder is a class that allows the easy configuration of
 *  connections. We decided to use the builder pattern because the connections
 *  have too many options to put them all in a single constructor. When you have
 *  passed all your options call connect() in order to receive a shared_ptr to a
 *  connection. Remember to use the "->" operator to access the connections
 *  members.
 */
class ConnectionBuilder {
  public:
    inline std::string host() const { return _conf._host; }
    ConnectionBuilder& host(std::string const&); // takes url in the form  (http|vst)[s]://(ip|hostname):port
                                                 // sets protocol host and port
    //ConnectionBuilder() = delete;
    //ConnectionBuilder(std::string const& s){
    //  host(s);
    //};

    // Create an connection and start opening it.
    std::shared_ptr<Connection> connect(EventLoopService& eventLoopService);

    // Set the authentication type of the connection
    inline AuthenticationType authenticationType() const { return _conf._authenticationType; }
    ConnectionBuilder& authenticationType(AuthenticationType t){ _conf._authenticationType = t; return *this; }
    // Set the username of the connection
    inline std::string user() const { return _conf._user; }
    ConnectionBuilder& user(std::string const& u){ _conf._user = u; return *this; }
    // Set the password of the connection
    inline std::string password() const { return _conf._password; }
    ConnectionBuilder& password(std::string const& p){ _conf._password = p; return *this; }
    // Set the maximum size for chunks (VST only)
    inline std::size_t maxChunkSize() const { return _conf._maxChunkSize; }
    ConnectionBuilder& maxChunkSize(std::size_t c){ _conf._maxChunkSize = c; return *this; }
    // Set the VST version to use (VST only)
    inline vst::VSTVersion vstVersion() const { return _conf._vstVersion; }
    ConnectionBuilder& vstVersion(vst::VSTVersion c){ _conf._vstVersion = c; return *this; }
    // Set a callback for connection failures that are not request specific.
    ConnectionBuilder& onFailure(ConnectionFailureCallback c){ _conf._onFailure = c; return *this; }

  private:
    detail::ConnectionConfiguration _conf;
};

}}}
#endif
