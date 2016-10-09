////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler
/// @author Achim Brandt
////////////////////////////////////////////////////////////////////////////////

#include "HttpCommTask.h"

#include "Basics/HybridLogicalClock.h"
#include "GeneralServer/GeneralServer.h"
#include "GeneralServer/GeneralServerFeature.h"
#include "GeneralServer/RestHandler.h"
#include "GeneralServer/RestHandlerFactory.h"
#include "Meta/conversion.h"
#include "VocBase/ticks.h"

using namespace arangodb;
using namespace arangodb::basics;
using namespace arangodb::rest;

size_t const HttpCommTask::MaximalHeaderSize = 2 * 1024 * 1024;       //    2 MB
size_t const HttpCommTask::MaximalBodySize = 1024 * 1024 * 1024;      // 1024 MB
size_t const HttpCommTask::MaximalPipelineSize = 1024 * 1024 * 1024;  // 1024 MB
size_t const HttpCommTask::RunCompactEvery = 500;

HttpCommTask::HttpCommTask(GeneralServer* server, TRI_socket_t sock,
                           ConnectionInfo&& info, double timeout)
    : Task("HttpCommTask"),
      GeneralCommTask(server, sock, std::move(info), timeout),
      _readPosition(0),
      _startPosition(0),
      _bodyPosition(0),
      _bodyLength(0),
      _readRequestBody(false),
      _allowMethodOverride(GeneralServerFeature::allowMethodOverride()),
      _denyCredentials(true),
      _newRequest(true),
      _requestType(rest::RequestType::ILLEGAL),  // TODO(fc) remove
      _fullUrl(),                                // TODO(fc) remove
      _origin(),                                 // TODO(fc) remove
      _sinceCompactification(0),
      _originalBodyLength(0) {  // TODO(fc) remove
  _protocol = "http";
  connectionStatisticsAgentSetHttp();  // this agent is inherited form
                                       // sockettask or task
  _agents.emplace(std::make_pair(1UL, RequestStatisticsAgent(true)));
}

void HttpCommTask::handleSimpleError(rest::ResponseCode code,
                                     uint64_t /* messageId */) {
  std::unique_ptr<GeneralResponse> response(new HttpResponse(code));
  addResponse(response.get());
}

void HttpCommTask::handleSimpleError(rest::ResponseCode code, int errorNum,
                                     std::string const& errorMessage,
                                     uint64_t /* messageId */) {
  std::unique_ptr<GeneralResponse> response(new HttpResponse(code));

  VPackBuilder builder;
  builder.openObject();
  builder.add(StaticStrings::Error, VPackValue(true));
  builder.add(StaticStrings::ErrorNum, VPackValue(errorNum));
  builder.add(StaticStrings::ErrorMessage, VPackValue(errorMessage));
  builder.add(StaticStrings::Code, VPackValue((int)code));
  builder.close();

  try {
    response->setPayload(builder.slice(), true, VPackOptions::Defaults);
    addResponse(response.get());
  } catch (std::exception const& ex) {
    LOG_TOPIC(WARN, Logger::COMMUNICATION)
        << "handleSimpleError received an exception, closing connection:"
        << ex.what();
    _clientClosed = true;
  } catch (...) {
    LOG_TOPIC(WARN, Logger::COMMUNICATION)
        << "handleSimpleError received an exception, closing connection";
    _clientClosed = true;
  }
}

void HttpCommTask::addResponse(HttpResponse* response) {
  _requestPending = false;
  _isChunked = false;

  // CORS response handling
  if (!_origin.empty()) {
    // the request contained an Origin header. We have to send back the
    // access-control-allow-origin header now
    LOG(TRACE) << "handling CORS response";

    response->setHeaderNC(StaticStrings::AccessControlExposeHeaders,
                          StaticStrings::ExposedCorsHeaders);

    // send back original value of "Origin" header
    response->setHeaderNC(StaticStrings::AccessControlAllowOrigin, _origin);

    // send back "Access-Control-Allow-Credentials" header
    response->setHeaderNC(StaticStrings::AccessControlAllowCredentials,
                          (_denyCredentials ? "false" : "true"));
  }

  // set "connection" header, keep-alive is the default
  response->setConnectionType(
      _closeRequested ? rest::ConnectionType::C_CLOSE
                      : rest::ConnectionType::C_KEEP_ALIVE);

  size_t const responseBodyLength = response->bodySize();

  // TODO(fc) should be handled by the response / request
  if (_requestType == rest::RequestType::HEAD) {
    // clear body if this is an HTTP HEAD request
    // HEAD must not return a body
    response->headResponse(responseBodyLength);
  }

  // reserve a buffer with some spare capacity
  auto buffer = std::make_unique<StringBuffer>(TRI_UNKNOWN_MEM_ZONE,
                                               responseBodyLength + 128, false);

  // TODO: move this to HttpResponse

  // write header
  response->writeHeader(buffer.get());

  // write body
  if (_requestType != rest::RequestType::HEAD) {
    if (_isChunked) {
      if (0 != responseBodyLength) {
        buffer->appendHex(response->body().length());
        buffer->appendText(TRI_CHAR_LENGTH_PAIR("\r\n"));
        buffer->appendText(response->body());
        buffer->appendText(TRI_CHAR_LENGTH_PAIR("\r\n"));
      }
    } else {
      buffer->appendText(response->body());
    }
  }

  buffer->ensureNullTerminated();

  if (!buffer->empty()) {
    LOG_TOPIC(TRACE, Logger::REQUESTS)
        << "\"http-request-response\",\"" << (void*)this << "\",\""
        << StringUtils::escapeUnicode(
               std::string(buffer->c_str(), buffer->length()))
        << "\"";
  }

  auto agent = getAgent(1);
  double const totalTime = agent->elapsedSinceReadStart();
               
  // append write buffer and statistics
  addWriteBuffer(std::move(buffer), agent);

  // and give some request information
  LOG_TOPIC(INFO, Logger::REQUESTS)
      << "\"http-request-end\",\"" << (void*)this << "\",\""
      << _connectionInfo.clientAddress << "\",\""
      << HttpRequest::translateMethod(_requestType) << "\",\""
      << HttpRequest::translateVersion(_protocolVersion) << "\","
      << static_cast<int>(response->responseCode()) << ","
      << _originalBodyLength << "," << responseBodyLength << ",\"" << _fullUrl
      << "\"," << Logger::FIXED(totalTime, 6);

  // clear body
  response->body().clear();
}

// reads data from the socket
bool HttpCommTask::processRead() {
  TRI_ASSERT(_readBuffer.c_str() != nullptr);

  if (_requestPending) {
    return false;
  }

  auto agent = getAgent(1UL);

  bool handleRequest = false;

  // still trying to read the header fields
  if (!_readRequestBody) {
    char const* ptr = _readBuffer.c_str() + _readPosition;
    char const* etr = _readBuffer.end();

    if (ptr == etr) {
      return false;
    }

    // starting a new request
    if (_newRequest) {
      // acquire a new statistics entry for the request
      agent->acquire();

#if USE_DEV_TIMERS
      if (RequestStatisticsAgent::_statistics != nullptr) {
        RequestStatisticsAgent::_statistics->_id = (void*)this;
      }
#endif

      _newRequest = false;
      _startPosition = _readPosition;
      _protocolVersion = rest::ProtocolVersion::UNKNOWN;
      _requestType = rest::RequestType::ILLEGAL;
      _fullUrl = "";
      _denyCredentials = true;

      _sinceCompactification++;
    }

    char const* end = etr - 3;

    // read buffer contents are way too small. we can exit here directly
    if (ptr >= end) {
      return false;
    }

    // request started
    agent->requestStatisticsAgentSetReadStart();

    // check for the end of the request
    for (; ptr < end; ptr++) {
      if (ptr[0] == '\r' && ptr[1] == '\n' && ptr[2] == '\r' &&
          ptr[3] == '\n') {
        break;
      }
    }

    // check if header is too large
    size_t headerLength = ptr - (_readBuffer.c_str() + _startPosition);

    if (headerLength > MaximalHeaderSize) {
      LOG(WARN) << "maximal header size is " << MaximalHeaderSize
                << ", request header size is " << headerLength;

      // header is too large
      handleSimpleError(rest::ResponseCode::REQUEST_HEADER_FIELDS_TOO_LARGE,
                        1);  // ID does not matter for http (http default is 1)

      return false;
    }

    // header is complete
    if (ptr < end) {
      _readPosition = ptr - _readBuffer.c_str() + 4;

      LOG(TRACE) << "HTTP READ FOR " << (void*)this << ": "
                 << std::string(_readBuffer.c_str() + _startPosition,
                                _readPosition - _startPosition);

      // check that we know, how to serve this request and update the connection
      // information, i. e. client and server addresses and ports and create a
      // request context for that request
      _incompleteRequest.reset(new HttpRequest(
          _connectionInfo, _readBuffer.c_str() + _startPosition,
          _readPosition - _startPosition, _allowMethodOverride));

      GeneralServerFeature::HANDLER_FACTORY->setRequestContext(
          _incompleteRequest.get());
      _incompleteRequest->setClientTaskId(_taskId);

      // check HTTP protocol version
      _protocolVersion = _incompleteRequest->protocolVersion();

      if (_protocolVersion != rest::ProtocolVersion::HTTP_1_0 &&
          _protocolVersion != rest::ProtocolVersion::HTTP_1_1) {
        handleSimpleError(rest::ResponseCode::HTTP_VERSION_NOT_SUPPORTED,
                          1);  // FIXME

        return false;
      }

      // check max URL length
      _fullUrl = _incompleteRequest->fullUrl();

      if (_fullUrl.size() > 16384) {
        handleSimpleError(rest::ResponseCode::REQUEST_URI_TOO_LONG,
                          1);  // FIXME
        return false;
      }

      // update the connection information, i. e. client and server addresses
      // and ports
      _incompleteRequest->setProtocol(_protocol);

      LOG(TRACE) << "server port " << _connectionInfo.serverPort
                 << ", client port " << _connectionInfo.clientPort;

      // set body start to current position
      _bodyPosition = _readPosition;
      _bodyLength = 0;

      // keep track of the original value of the "origin" request header (if
      // any), we need this value to handle CORS requests
      _origin = _incompleteRequest->header(StaticStrings::Origin);

      if (!_origin.empty()) {
        // default is to allow nothing
        _denyCredentials = true;

        // if the request asks to allow credentials, we'll check against the
        // configured whitelist of origins
        std::vector<std::string> const& accessControlAllowOrigins =
            GeneralServerFeature::accessControlAllowOrigins();

        if (!accessControlAllowOrigins.empty()) {
          if (accessControlAllowOrigins[0] == "*") {
            // special case: allow everything
            _denyCredentials = false;
          } else if (!_origin.empty()) {
            // copy origin string
            if (_origin[_origin.size() - 1] == '/') {
              // strip trailing slash
              auto result = std::find(accessControlAllowOrigins.begin(),
                                      accessControlAllowOrigins.end(),
                                      _origin.substr(0, _origin.size() - 1));
              _denyCredentials = (result == accessControlAllowOrigins.end());
            } else {
              auto result = std::find(accessControlAllowOrigins.begin(),
                                      accessControlAllowOrigins.end(), _origin);
              _denyCredentials = (result == accessControlAllowOrigins.end());
            }
          } else {
            TRI_ASSERT(_denyCredentials);
          }
        }
      }

      // store the original request's type. we need it later when responding
      // (original request object gets deleted before responding)
      _requestType = _incompleteRequest->requestType();

      agent->requestStatisticsAgentSetRequestType(_requestType);

      // handle different HTTP methods
      switch (_requestType) {
        case rest::RequestType::GET:
        case rest::RequestType::DELETE_REQ:
        case rest::RequestType::HEAD:
        case rest::RequestType::OPTIONS:
        case rest::RequestType::POST:
        case rest::RequestType::PUT:
        case rest::RequestType::PATCH: {
          // technically, sending a body for an HTTP DELETE request is not
          // forbidden, but it is not explicitly supported
          bool const expectContentLength =
              (_requestType == rest::RequestType::POST ||
               _requestType == rest::RequestType::PUT ||
               _requestType == rest::RequestType::PATCH ||
               _requestType == rest::RequestType::OPTIONS ||
               _requestType == rest::RequestType::DELETE_REQ);

          if (!checkContentLength(_incompleteRequest.get(),
                                  expectContentLength)) {
            return false;
          }

          if (_bodyLength == 0) {
            handleRequest = true;
          }

          break;
        }

        default: {
          // bad request, method not allowed
          handleSimpleError(rest::ResponseCode::METHOD_NOT_ALLOWED, 1);
          return false;
        }
      }

      // check for a 100-continue
      if (_readRequestBody) {
        bool found;
        std::string const& expect =
            _incompleteRequest->header(StaticStrings::Expect, found);

        if (found && StringUtils::trim(expect) == "100-continue") {
          LOG(TRACE) << "received a 100-continue request";

          auto buffer = std::make_unique<StringBuffer>(TRI_UNKNOWN_MEM_ZONE);
          buffer->appendText(
              TRI_CHAR_LENGTH_PAIR("HTTP/1.1 100 (Continue)\r\n\r\n"));
          buffer->ensureNullTerminated();

          addWriteBuffer(std::move(buffer));
        }
      }
    } else {
      size_t l = (_readBuffer.end() - _readBuffer.c_str());

      if (_startPosition + 4 <= l) {
        _readPosition = l - 4;
      }
    }
  }

  // readRequestBody might have changed, so cannot use else
  if (_readRequestBody) {
    if (_readBuffer.length() - _bodyPosition < _bodyLength) {
      armKeepAliveTimeout();

      // let client send more
      return false;
    }
      
    bool handled = false; 
    std::string const& encoding = _incompleteRequest->header(StaticStrings::ContentEncoding);
    if (!encoding.empty()) {
      if (encoding == "gzip") {
        std::string uncompressed;
        if (!StringUtils::gzipUncompress(_readBuffer.c_str() + _bodyPosition, _bodyLength, uncompressed)) {
          handleSimpleError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER, "gzip decoding error", 1);
          return false;
        }
        _incompleteRequest->setBody(uncompressed.c_str(), uncompressed.size());
        handled = true;
      } else if (encoding == "deflate") {
        std::string uncompressed;
        if (!StringUtils::gzipDeflate(_readBuffer.c_str() + _bodyPosition, _bodyLength, uncompressed)) {
          handleSimpleError(rest::ResponseCode::BAD, TRI_ERROR_BAD_PARAMETER, "gzip deflate error", 1);
          return false;
        }
        _incompleteRequest->setBody(uncompressed.c_str(), uncompressed.size());
        handled = true;
      }
    }

    if (!handled) {
      // read "bodyLength" from read buffer and add this body to "httpRequest"
      _incompleteRequest->setBody(_readBuffer.c_str() + _bodyPosition,
                                  _bodyLength);
    }

    LOG(TRACE) << std::string(_readBuffer.c_str() + _bodyPosition, _bodyLength);

    // remove body from read buffer and reset read position
    _readRequestBody = false;
    handleRequest = true;
  }

  // .............................................................................
  // request complete
  //
  // we have to delete request in here or pass it to a handler, which will
  // delete
  // it
  // .............................................................................

  if (!handleRequest) {
    return false;
  }

  agent->requestStatisticsAgentSetReadEnd();
  agent->requestStatisticsAgentAddReceivedBytes(_bodyPosition - _startPosition +
                                                _bodyLength);

  bool const isOptionsRequest = (_requestType == rest::RequestType::OPTIONS);
  resetState();

  // .............................................................................
  // keep-alive handling
  // .............................................................................

  // header value can have any case. we'll lower-case it now
  std::string connectionType = StringUtils::tolower(
      _incompleteRequest->header(StaticStrings::Connection));

  if (connectionType == "close") {
    // client has sent an explicit "Connection: Close" header. we should close
    // the connection
    LOG(DEBUG) << "connection close requested by client";
    _closeRequested = true;
  } else if (_incompleteRequest->isHttp10() && connectionType != "keep-alive") {
    // HTTP 1.0 request, and no "Connection: Keep-Alive" header sent
    // we should close the connection
    LOG(DEBUG) << "no keep-alive, connection close requested by client";
    _closeRequested = true;
  } else if (_keepAliveTimeout <= 0.0) {
    // if keepAliveTimeout was set to 0.0, we'll close even keep-alive
    // connections immediately
    LOG(DEBUG) << "keep-alive disabled by admin";
    _closeRequested = true;
  }

  // we keep the connection open in all other cases (HTTP 1.1 or Keep-Alive
  // header sent)

  // .............................................................................
  // authenticate
  // .............................................................................

  rest::ResponseCode authResult = authenticateRequest(_incompleteRequest.get());

  // authenticated or an OPTIONS request. OPTIONS requests currently go
  // unauthenticated
  if (authResult == rest::ResponseCode::OK || isOptionsRequest) {
    // handle HTTP OPTIONS requests directly
    if (isOptionsRequest) {
      processCorsOptions(std::move(_incompleteRequest));
    } else {
      processRequest(std::move(_incompleteRequest));
    }
  }
  // not found
  else if (authResult == rest::ResponseCode::NOT_FOUND) {
    handleSimpleError(authResult, TRI_ERROR_ARANGO_DATABASE_NOT_FOUND,
                      TRI_errno_string(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND), 1);
  }
  // forbidden
  else if (authResult == rest::ResponseCode::FORBIDDEN) {
    handleSimpleError(authResult, TRI_ERROR_USER_CHANGE_PASSWORD,
                      "change password", 1);
  }
  // not authenticated
  else {
    HttpResponse response(rest::ResponseCode::UNAUTHORIZED);
    std::string realm = "Bearer token_type=\"JWT\", realm=\"ArangoDB\"";

    response.setHeaderNC(StaticStrings::WwwAuthenticate, std::move(realm));

    processResponse(&response);
  }

  _incompleteRequest.reset(nullptr);
  return true;
}

void HttpCommTask::processRequest(std::unique_ptr<HttpRequest> request) {
  {
    LOG_TOPIC(DEBUG, Logger::REQUESTS)
        << "\"http-request-begin\",\"" << (void*)this << "\",\""
        << _connectionInfo.clientAddress << "\",\""
        << HttpRequest::translateMethod(_requestType) << "\",\""
        << HttpRequest::translateVersion(_protocolVersion) << "\"," << _fullUrl
        << "\"";

    std::string const& body = request->body();

    if (!body.empty()) {
      LOG_TOPIC(DEBUG, Logger::REQUESTS)
          << "\"http-request-body\",\"" << (void*)this << "\",\""
          << (StringUtils::escapeUnicode(body)) << "\"";
    }
  }

  // check for an HLC time stamp
  bool found;
  std::string const& timeStamp =
      request->header(StaticStrings::HLCHeader, found);

  if (found) {
    uint64_t timeStampInt =
        arangodb::basics::HybridLogicalClock::decodeTimeStampWithCheck(
            timeStamp);
    if (timeStampInt != 0) {
      TRI_HybridLogicalClock(timeStampInt);
    }
  }

  // create a handler and execute
  std::unique_ptr<GeneralResponse> response(
      new HttpResponse(rest::ResponseCode::SERVER_ERROR));
  response->setContentType(request->contentTypeResponse());
  response->setContentTypeRequested(request->contentTypeResponse());

  executeRequest(std::move(request), std::move(response));
}

void HttpCommTask::finishedChunked() {
  auto buffer = std::make_unique<StringBuffer>(TRI_UNKNOWN_MEM_ZONE, 6, true);
  buffer->appendText(TRI_CHAR_LENGTH_PAIR("0\r\n\r\n"));
  buffer->ensureNullTerminated();

  _isChunked = false;
  _requestPending = false;

  addWriteBuffer(std::move(buffer));
}

////////////////////////////////////////////////////////////////////////////////
/// check the content-length header of a request and fail it is broken
////////////////////////////////////////////////////////////////////////////////

bool HttpCommTask::checkContentLength(HttpRequest* request,
                                      bool expectContentLength) {
  int64_t const bodyLength = request->contentLength();

  if (bodyLength < 0) {
    // bad request, body length is < 0. this is a client error
    handleSimpleError(rest::ResponseCode::LENGTH_REQUIRED);
    return false;
  }

  if (!expectContentLength && bodyLength > 0) {
    // content-length header was sent but the request method does not support
    // that
    // we'll warn but read the body anyway
    LOG(WARN) << "received HTTP GET/HEAD request with content-length, this "
                 "should not happen";
  }

  if ((size_t)bodyLength > MaximalBodySize) {
    LOG(WARN) << "maximal body size is " << MaximalBodySize
              << ", request body size is " << bodyLength;

    // request entity too large
    handleSimpleError(rest::ResponseCode::REQUEST_ENTITY_TOO_LARGE,
                      0);  // FIXME
    return false;
  }

  // set instance variable to content-length value
  _bodyLength = (size_t)bodyLength;
  _originalBodyLength = _bodyLength;

  if (_bodyLength > 0) {
    // we'll read the body
    _readRequestBody = true;
  }

  // everything's fine
  return true;
}

void HttpCommTask::processCorsOptions(std::unique_ptr<HttpRequest> request) {
  HttpResponse response(rest::ResponseCode::OK);

  response.setHeaderNC(StaticStrings::Allow, StaticStrings::CorsMethods);

  if (!_origin.empty()) {
    LOG(TRACE) << "got CORS preflight request";
    std::string const allowHeaders = StringUtils::trim(
        request->header(StaticStrings::AccessControlRequestHeaders));

    // send back which HTTP methods are allowed for the resource
    // we'll allow all
    response.setHeaderNC(StaticStrings::AccessControlAllowMethods,
                         StaticStrings::CorsMethods);

    if (!allowHeaders.empty()) {
      // allow all extra headers the client requested
      // we don't verify them here. the worst that can happen is that the client
      // sends some broken headers and then later cannot access the data on the
      // server. that's a client problem.
      response.setHeaderNC(StaticStrings::AccessControlAllowHeaders,
                           allowHeaders);

      LOG(TRACE) << "client requested validation of the following headers: "
                 << allowHeaders;
    }

    // set caching time (hard-coded value)
    response.setHeaderNC(StaticStrings::AccessControlMaxAge,
                         StaticStrings::N1800);
  }

  processResponse(&response);
}

void HttpCommTask::handleChunk(char const* data, size_t len) {
  if (!_isChunked) {
    return;
  }

  if (0 == len) {
    finishedChunked();
  } else {
    std::unique_ptr<StringBuffer> buffer(
        new StringBuffer(TRI_UNKNOWN_MEM_ZONE, len));

    buffer->appendHex(len);
    buffer->appendText(TRI_CHAR_LENGTH_PAIR("\r\n"));
    buffer->appendText(data, len);
    buffer->appendText(TRI_CHAR_LENGTH_PAIR("\r\n"));

    addWriteBuffer(std::move(buffer));
  }
}

std::unique_ptr<GeneralResponse> HttpCommTask::createResponse(
    rest::ResponseCode responseCode, uint64_t /* messageId */) {
  return std::unique_ptr<GeneralResponse>(new HttpResponse(responseCode));
}

void HttpCommTask::resetState() {
  _requestPending = true;

  bool compact = false;

  if (_sinceCompactification > RunCompactEvery) {
    compact = true;
  } else if (_readBuffer.length() > MaximalPipelineSize) {
    compact = true;
  }

  if (compact) {
    _readBuffer.erase_front(_bodyPosition + _bodyLength);

    _sinceCompactification = 0;
    _readPosition = 0;
  } else {
    _readPosition = _bodyPosition + _bodyLength;

    if (_readPosition == _readBuffer.length()) {
      _sinceCompactification = 0;
      _readPosition = 0;
      _readBuffer.reset();
    }
  }

  _bodyPosition = 0;
  _bodyLength = 0;

  _newRequest = true;
  _readRequestBody = false;
}

rest::ResponseCode HttpCommTask::authenticateRequest(HttpRequest* request) {
  auto context = request->requestContext();

  if (context == nullptr) {
    bool res =
        GeneralServerFeature::HANDLER_FACTORY->setRequestContext(request);

    if (!res) {
      return rest::ResponseCode::NOT_FOUND;
    }

    context = request->requestContext();
  }

  if (context == nullptr) {
    return rest::ResponseCode::SERVER_ERROR;
  }

  return context->authenticate();
}
