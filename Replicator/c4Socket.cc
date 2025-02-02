//
// c4Socket.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "fleece/Fleece.hh"
#include "c4ExceptionUtils.hh"
#include "c4Private.h"
#include "c4Socket.hh"
#include "c4Socket+Internal.hh"
#include "Address.hh"
#include "Error.hh"
#include "Headers.hh"
#include "Logging.hh"
#include "WebSocketImpl.hh"
#include "StringUtil.hh"
#include <atomic>
#include <exception>

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::net;
using namespace litecore::repl;
using namespace websocket;


static C4SocketFactory* sRegisteredFactory;
static C4SocketImpl::InternalFactory sRegisteredInternalFactory;


C4Socket::~C4Socket() = default;


void C4Socket::registerFactory(const C4SocketFactory &factory) {
    Assert(factory.write != nullptr && factory.completedReceive != nullptr);
    if (factory.framing == kC4NoFraming)
        Assert(factory.close == nullptr && factory.requestClose != nullptr);
    else
        Assert(factory.close != nullptr && factory.requestClose == nullptr);

    if (sRegisteredFactory)
        throw std::logic_error("c4socket_registerFactory can only be called once");
    sRegisteredFactory = new C4SocketFactory(factory);
}


C4Socket* C4Socket::fromNative(const C4SocketFactory &factory,
                               void *nativeHandle,
                               const C4Address &address)
{
    return new C4SocketImpl(address.toURL(), Role::Server, {}, &factory, nativeHandle);
}


#pragma mark - C4SOCKETIMPL:


namespace litecore::repl {


    void C4SocketImpl::registerInternalFactory(C4SocketImpl::InternalFactory f) {
        sRegisteredInternalFactory = f;
    }


    Retained<WebSocket> CreateWebSocket(websocket::URL url,
                                        alloc_slice options,
                                        C4Database *database,
                                        const C4SocketFactory *factory,
                                        void *nativeHandle)
    {
        if (!factory)
            factory = sRegisteredFactory;
        
        if (factory) {
            return new C4SocketImpl(url, Role::Client, options, factory, nativeHandle);
        } else if (sRegisteredInternalFactory) {
            Assert(!nativeHandle);
            return sRegisteredInternalFactory(url, options, database);
        } else {
            throw std::logic_error("No default C4SocketFactory registered; call c4socket_registerFactory())");
        }
    }


    static const C4SocketFactory& effectiveFactory(const C4SocketFactory *f) {
        return f ? *f : C4SocketImpl::registeredFactory();
    }


    WebSocketImpl::Parameters C4SocketImpl::convertParams(slice c4SocketOptions) {
        WebSocketImpl::Parameters params = {};
        params.options = AllocedDict(c4SocketOptions);
        params.webSocketProtocols = params.options[kC4SocketOptionWSProtocols].asString();
        params.heartbeatSecs = (int)params.options[kC4ReplicatorHeartbeatInterval].asInt();
        return params;
    }


    C4SocketImpl::C4SocketImpl(websocket::URL url,
                               Role role,
                               alloc_slice options,
                               const C4SocketFactory *factory_,
                               void *nativeHandle_)
    :WebSocketImpl(url,
                   role,
                   effectiveFactory(factory_).framing != kC4NoFraming,
                   convertParams(options))
    ,_factory(effectiveFactory(factory_))
    {
        nativeHandle = nativeHandle_;
    }

    C4SocketImpl::~C4SocketImpl() {
        if (_factory.dispose)
            _factory.dispose(this);
    }


    WebSocket* WebSocketFrom(C4Socket *c4sock) {
        return c4sock ? (C4SocketImpl*)c4sock : nullptr;
    }


    const C4SocketFactory& C4SocketImpl::registeredFactory() {
        if (!sRegisteredFactory)
            throw std::logic_error("No default C4SocketFactory registered; call c4socket_registerFactory())");
        return *sRegisteredFactory;
    }


#pragma mark - WEBSOCKETIMPL OVERRIDES:


    void C4SocketImpl::connect() {
        WebSocketImpl::connect();
        if (_factory.open) {
            net::Address c4addr(url());
            _factory.open(this, &c4addr, options().data(), _factory.context);
        }
    }

    void C4SocketImpl::requestClose(int status, fleece::slice message) {
        _factory.requestClose(this, status, message);
    }

    void C4SocketImpl::closeSocket() {
        _factory.close(this);
    }

    void C4SocketImpl::closeWithException() {
        C4Error error = C4Error::fromCurrentException();
        WarnError("Closing socket due to C++ exception: %s\n%s",
                  error.description().c_str(), error.backtrace().c_str());
        close(kCodeUnexpectedCondition, "Internal exception"_sl);
    }

    void C4SocketImpl::sendBytes(alloc_slice bytes) {
        _factory.write(this, C4SliceResult(bytes));
    }

    void C4SocketImpl::receiveComplete(size_t byteCount) {
        _factory.completedReceive(this, byteCount);
    }


#pragma mark - C4SOCKET C++ API:


    void C4SocketImpl::gotHTTPResponse(int status, slice responseHeadersFleece) {
        try {
            Headers headers(responseHeadersFleece);
            WebSocketImpl::gotHTTPResponse(status, headers);
        } catch (...) {closeWithException();}
    }


    void C4SocketImpl::opened() {
        try {
            WebSocketImpl::onConnect();
        } catch (...) {closeWithException();}
    }


    void C4SocketImpl::closeRequested(int status, slice message) {
        try {
            WebSocketImpl::onCloseRequested(status, message);
        } catch (...) {closeWithException();}
    }


    void C4SocketImpl::closed(C4Error error) {
        try {
            alloc_slice message = c4error_getMessage(error);
            CloseStatus status {kUnknownError, error.code, message};
            if (error.code == 0) {
                status.reason = kWebSocketClose;
                status.code = kCodeNormal;
            } else if (error.domain == WebSocketDomain)
                status.reason = kWebSocketClose;
            else if (error.domain == POSIXDomain)
                status.reason = kPOSIXError;
            else if (error.domain == NetworkDomain)
                status.reason = kNetworkError;

            WebSocketImpl::onClose(status);
        } catch (...) {closeWithException();}
    }


    void C4SocketImpl::completedWrite(size_t byteCount) {
        try {
            WebSocketImpl::onWriteComplete(byteCount);
        } catch (...) {closeWithException();}
    }


    void C4SocketImpl::received(slice data) {
        try {
            WebSocketImpl::onReceive(data);
        } catch (...) {closeWithException();}
    }

}
