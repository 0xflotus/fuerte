var fuerte = require('.')
var vpack = require('node-velocypack')

// create server
var builder = new fuerte.ConnectionBuilder()
var connection_vst = builder.host("vst://127.0.0.1:8530").connect();
var connection_http = builder.host("http://127.0.0.1:8529").connect();

var request = new fuerte.Request();
request.setRestVerb("get");
request.setPath("/_api/version");

var onError = function(code, req, res){
  console.log("\n#### error ####\n")
}

var onSuccess = function(req, res){
  console.log("\n#### succes ####\n")
  // we maybe return a pure js objects (req/res)
  if(res.notNull()){
    console.log("response code: " + res.getResponseCode())
    console.log("payload raw vpack: " + res.payload())
  }
}

console.log("queue 1")
connection_vst.sendRequest(request, onError, onSuccess);
connection_http.sendRequest(request, onError, onSuccess);
fuerte.run();
console.log("1 done")
console.log("------------------------------------------")

console.log("queue 2")
connection_vst.sendRequest(request, onError, onSuccess);
connection_vst.sendRequest(request, onError, onSuccess);
console.log("queue 3")
connection_vst.sendRequest(request, onError, onSuccess);
connection_http.sendRequest(request, onError, onSuccess);
fuerte.run();
console.log("2,3 done")
console.log("------------------------------------------")
console.log("queue 4")
var requestCursor = new fuerte.Request();
var slice = vpack.encode({"query": "FOR x IN 1..5 RETURN x"});
requestCursor.setRestVerb("post");
requestCursor.setPath("/_api/cursor");
requestCursor.addVPack(slice);
connection_vst.sendRequest(requestCursor, onError, onSuccess);
connection_http.sendRequest(requestCursor, onError, onSuccess);
fuerte.run();
console.log("4 done")
console.log("------------------------------------------")
console.log("queue 5")
var requestCursor = new fuerte.Request();
var slice = vpack.encode({});
requestCursor.setRestVerb("post");
requestCursor.setPath("/_api/document/_users");
requestCursor.addVPack(slice);
connection_vst.sendRequest(requestCursor, onError, onSuccess);
connection_http.sendRequest(requestCursor, onError, onSuccess);
fuerte.run();
console.log("5 done")
console.log("------------------------------------------")
