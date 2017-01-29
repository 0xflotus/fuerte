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
////////////////////////////////////////////////////////////////////////////////
#include "test_main.h"
#include <fuerte/arangocxx.h>
#include <fuerte/loop.h>

namespace f = ::arangodb::fuerte;

class ConnectionBasicF : public ::testing::Test {
 protected:
  ConnectionBasicF(){
    _server = "vst://127.0.0.1:8529";
  }
  virtual ~ConnectionBasicF() noexcept {}

  virtual void SetUp() override {
    f::ConnectionBuilder cbuilder;
    cbuilder.host(_server);
    _connection = cbuilder.connect();
  }

  virtual void TearDown() override {
    _connection.reset();
  }

  std::shared_ptr<f::Connection> _connection;

 private:
  std::string _server;
  std::string _port;

};

namespace fu = ::arangodb::fuerte;

TEST_F(ConnectionBasicF, ApiVersionSync){
  auto request = fu::createRequest(fu::RestVerb::Get, "/_api/version");
  auto result = _connection->sendRequest(std::move(request));
  auto slice = result->slices().front();
  auto version = slice.get("version").copyString();
  auto server = slice.get("server").copyString();
  ASSERT_TRUE(server == std::string("arango")) << server << " == arango";
  ASSERT_TRUE(version[0] == '3');
}

TEST_F(ConnectionBasicF, ApiVersionASync){
  auto request = fu::createRequest(fu::RestVerb::Get, "/_api/version");
  fu::OnErrorCallback onError = [](fu::Error error, std::unique_ptr<fu::Request> req, std::unique_ptr<fu::Response> res){
    ASSERT_TRUE(false) << fu::to_string(fu::intToError(error));
  };
  fu::OnSuccessCallback onSuccess = [](std::unique_ptr<fu::Request> req, std::unique_ptr<fu::Response> res){
    auto slice = res->slices().front();
    auto version = slice.get("version").copyString();
    auto server = slice.get("server").copyString();
    ASSERT_TRUE(server == std::string("arango")) << server << " == arango";
    ASSERT_TRUE(version[0] == '3');
  };
  _connection->sendRequest(std::move(request),onError,onSuccess);
  fu::run();
}

TEST_F(ConnectionBasicF, ApiVersionSync20){
  auto request = fu::createRequest(fu::RestVerb::Get, "/_api/version");
  fu::Request req = *request;
  for(int i = 0; i < 20; i++){
    auto result = _connection->sendRequest(req);
    auto slice = result->slices().front();
    auto version = slice.get("version").copyString();
    auto server = slice.get("server").copyString();
    ASSERT_TRUE(server == std::string("arango")) << server << " == arango";
    ASSERT_TRUE(version[0] == '3');
  }
}

TEST_F(ConnectionBasicF, ApiVersionASync20){
  auto request = fu::createRequest(fu::RestVerb::Get, "/_api/version");
  fu::OnErrorCallback onError = [](fu::Error error, std::unique_ptr<fu::Request> req, std::unique_ptr<fu::Response> res){
    ASSERT_TRUE(false) << fu::to_string(fu::intToError(error));
  };
  fu::OnSuccessCallback onSuccess = [](std::unique_ptr<fu::Request> req, std::unique_ptr<fu::Response> res){
    auto slice = res->slices().front();
    auto version = slice.get("version").copyString();
    auto server = slice.get("server").copyString();
    ASSERT_TRUE(server == std::string("arango")) << server << " == arango";
    ASSERT_TRUE(version[0] == '3');
  };
  fu::Request req = *request;
  for(int i = 0; i < 20; i++){
    _connection->sendRequest(req,onError,onSuccess);
  }
  fu::run();
}

