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

#include <fuerte/connection.h>
#include <fuerte/FuerteLogger.h>
#include <fuerte/waitgroup.h>

namespace arangodb { namespace fuerte { inline namespace v1 {

// Deconstructor
Connection::~Connection() { FUERTE_LOG_DEBUG << "Destroying Connection" << std::endl; }

// sendRequest and wait for it to finished.
std::unique_ptr<Response> Connection::sendRequest(std::unique_ptr<Request> request){
  FUERTE_LOG_TRACE << "start sync request" << std::endl;

  WaitGroup wg;
  auto rv = std::unique_ptr<Response>(nullptr);
  ::arangodb::fuerte::v1::Error error = 0;

  auto cb = [&](::arangodb::fuerte::v1::Error e, std::unique_ptr<Request> request, std::unique_ptr<Response> response){
    WaitGroupDone done(wg);
    FUERTE_LOG_TRACE << "sendRequest (sync): onError" << std::endl;
    rv = std::move(response);
    error = e;
  };

  {
    // Start asynchronous request
    wg.add();
    sendRequest(std::move(request), cb);

    // Wait for request to finish.
    FUERTE_LOG_TRACE << "sendRequest (sync): before wait" << std::endl;
    wg.wait();
  }

  FUERTE_LOG_TRACE << "sendRequest (sync): done" << std::endl;

  if (error != 0) {
    throw intToError(error);
  }

  return std::move(rv);
}

}}}
