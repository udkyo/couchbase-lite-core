//
// c4Socket.h
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

#pragma once
#include "c4SocketTypes.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

    /** \defgroup Socket  Replication Socket Provider API
        @{ */


    // NOTE: C4Socket used to be a concrete struct containing a single field `nativeHandle`.
    // As part of creating the C++ API, this struct declaration was removed so it could be
    // declared in c4Struct.hh as a real C++ object.
    // To fix client code that accessed `nativeHandle` directly, call `c4Socket_setNativeHandle`
    // and/or `c4Socket_setNativeHandle` instead.
    

    /** One-time registration of socket callbacks. Must be called before using any socket-based
        API including the replicator. Do not call multiple times. */
    void c4socket_registerFactory(C4SocketFactory factory) C4API;

    /** Associates an opaque "native handle" with this object. You can use this to store whatever
        you need to represent the socket's implementation, like a file descriptor. */
    void c4Socket_setNativeHandle(C4Socket*, void* C4NULLABLE) C4API;

    /** Returns the opaque "native handle" associated with this object. */
    void* C4NULLABLE c4Socket_getNativeHandle(C4Socket*) C4API;

    /** Notification that a socket has received an HTTP response, with the given headers (encoded
        as a Fleece dictionary.) This should be called just before c4socket_opened or
        c4socket_closed.
        @param socket  The socket being opened.
        @param httpStatus  The HTTP/WebSocket status code from the peer; expected to be 200 if the
            connection is successful, else an HTTP status >= 300 or WebSocket status >= 1000.
        @param responseHeadersFleece  The HTTP response headers, encoded as a Fleece dictionary
            whose keys are the header names (with normalized case) and values are header values
            as strings. */
    void c4socket_gotHTTPResponse(C4Socket *socket,
                                  int httpStatus,
                                  C4Slice responseHeadersFleece) C4API;

    /** Notifies LiteCore that a socket has opened, i.e. a C4SocketFactory.open request has completed
        successfully.
        @param socket  The socket. */
    void c4socket_opened(C4Socket *socket) C4API;

    /** Notifies LiteCore that a socket has finished closing, or disconnected, or failed to open.
        - If this is a normal close in response to a C4SocketFactory.close request, the error
          parameter should have a code of 0.
        - If it's a socket-level error, set the C4Error appropriately.
        - If it's a WebSocket-level close (when the factory's `framing` equals to `kC4NoFraming`),
          set the error domain to WebSocketDomain and the code to the WebSocket status code.
        @param socket  The socket.
        @param errorIfAny  the status of the close; see description above. */
    void c4socket_closed(C4Socket *socket, C4Error errorIfAny) C4API;

    /** Notifies LiteCore that the peer has requested to close the socket using the WebSocket protocol.
        (Should only be called by sockets whose factory's `framing` equals to `kC4NoFraming`.)
        LiteCore will call the factory's requestClose callback in response when it's ready to
        acknowledge the close.
        @param socket  The socket.
        @param  status  The WebSocket status sent by the peer, typically 1000.
        @param  message  An optional human-readable message sent by the peer. */
    void c4socket_closeRequested(C4Socket *socket, int status, C4String message) C4API;

    /** Notifies LiteCore that a C4SocketFactory.write request has been completed, i.e. the bytes
        have been written to the socket.
        @param socket  The socket.
        @param byteCount  The number of bytes that were written. */
    void c4socket_completedWrite(C4Socket *socket, size_t byteCount) C4API;

    /** Notifies LiteCore that data was received from the socket. If the factory's
        `framing` equals to `kC4NoFraming`, the data must be a single complete message; otherwise it's
        raw bytes that will be un-framed by LiteCore.
        LiteCore will acknowledge when it's received and processed the data, by calling
        C4SocketFactory.completedReceive. For flow-control purposes, the client should keep track
        of the number of unacknowledged bytes, and stop reading from the underlying stream if that
        grows too large.
        @param socket  The socket.
        @param data  The data received, either a message or raw bytes. */
    void c4socket_received(C4Socket *socket, C4Slice data) C4API;


    /** Constructs a C4Socket from a "native handle", whose interpretation is up to the
        C4SocketFactory.  This is used by listeners to handle an incoming replication connection.
        @param factory  The C4SocketFactory that will manage the socket.
        @param nativeHandle  A value known to the factory that represents the underlying socket,
            such as a file descriptor or a native object pointer.
        @param address  The address of the remote peer making the connection.
        @return  A new C4Socket initialized with the `nativeHandle`. */
    C4Socket* c4socket_fromNative(C4SocketFactory factory,
                                  void *nativeHandle,
                                  const C4Address *address) C4API;


    /** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
