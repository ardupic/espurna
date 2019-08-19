
#pragma once

#include <functional>
#include <ESPAsyncTCP.h>

#ifndef ASYNC_HTTP_DEBUG
#define ASYNC_HTTP_DEBUG(...)  //DEBUG_PORT.printf(__VA_ARGS__)
#endif

// <method> <path> <host> <len>
const char HTTP_REQUEST_TEMPLATE[] PROGMEM =
    "%s %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "User-Agent: ESPurna\r\n"
    "Connection: close\r\n"
    "Content-Type: application/x-www-form-urlencoded\r\n"
    "Content-Length: %u\r\n"
    "\r\n";

struct AsyncHttpError {

    enum error_t {
        EMPTY,
        CLIENT_ERROR,
        REQUEST_TIMEOUT,
        NETWORK_TIMEOUT,
    };

    const error_t error;
    const String data;

    AsyncHttpError() :
        error(EMPTY), data(0)
    {}

    AsyncHttpError(AsyncHttpError&) = default;

    AsyncHttpError(const error_t& err, const String& data) :
        error(err), data(data)
    {}

    bool operator==(const error_t& err) const {
        return err == error;
    }

    bool operator==(const AsyncHttpError& obj) const {
        return obj.error == error;
    }

};

class AsyncHttp {

    public:

        AsyncClient client;

        enum class state_t : uint8_t {
            NONE,
            HEADERS,
            BODY
        };

        using on_connected_f = std::function<void(AsyncHttp*)>;
        using on_status_f = std::function<bool(AsyncHttp*, uint16_t status)>;
        using on_disconnected_f = std::function<void(AsyncHttp*)>;
        using on_error_f = std::function<void(AsyncHttp*, const AsyncHttpError&)>;
        using on_body_f = std::function<void(AsyncHttp*, uint8_t*, size_t)>;

        state_t state = state_t::NONE;
        AsyncHttpError::error_t last_error;

        on_connected_f on_connected;
        on_disconnected_f on_disconnected;

        on_status_f on_status;
        on_error_f on_error;

        on_body_f on_body;

        String method;
        String path;

        String host;
        uint16_t port;

        String data; // TODO: generic data source, feed chunks of (bytes, len) and call us back when done

        uint32_t ts;
        uint32_t timeout = 5000;

        bool connected = false;
        bool connecting = false;

    protected:

        static AsyncHttpError _timeoutError(AsyncHttpError::error_t error, const __FlashStringHelper* message, uint32_t ts) {
            String data;
            data.reserve(32);
            data += message;
            data += ' ';
            data += String(ts);
            return {error, data};
        }

        static void _onDisconnect(void* http_ptr, AsyncClient*) {
            AsyncHttp* http = static_cast<AsyncHttp*>(http_ptr);
            if (http->on_disconnected) http->on_disconnected(http); 
            http->data = "";
            http->ts = 0;
            http->connected = false;
            http->connecting = false;
            http->state = AsyncHttp::state_t::NONE;
        }

        static void _onTimeout(void* http_ptr, AsyncClient* client, uint32_t time) {
            client->close(true);

            AsyncHttp* http = static_cast<AsyncHttp*>(http_ptr);
            http->last_error = AsyncHttpError::NETWORK_TIMEOUT;
            if (http->on_error) http->on_error(http, _timeoutError(AsyncHttpError::NETWORK_TIMEOUT, F("Network timeout after"), time));
            // TODO: close connection when acks are missing?
        }

        static void _onPoll(void* http_ptr, AsyncClient*) {
            AsyncHttp* http = static_cast<AsyncHttp*>(http_ptr);
            const auto diff = millis() - http->ts;
            if (diff > http->timeout) {
                if (http->on_error) http->on_error(http, _timeoutError(AsyncHttpError::REQUEST_TIMEOUT, F("No response after"), diff));
                http->client.close(true);
            }
        }

        static void _onData(void* http_ptr, AsyncClient* client, void* response, size_t len) {

            AsyncHttp* http = static_cast<AsyncHttp*>(http_ptr);
            http->ts = millis();

            char * p = nullptr;

            do {

                p = nullptr;

                switch (http->state) {
                    case AsyncHttp::state_t::NONE:
                        {
                            if (len < strlen("HTTP/1.1 ... OK") + 1) {
                                ASYNC_HTTP_DEBUG("err | not enough len\n");
                                client->close(true);
                                return;
                            }
                            p = strnstr(reinterpret_cast<const char *>(response), "HTTP/1.", len);
                            if (!p) {
                                ASYNC_HTTP_DEBUG("err | not http\n");
                                client->close(true);
                                return;
                            }

                            p += strlen("HTTP/1.");
                            if ((p[0] != '1') && (p[0] != '0')) {
                                ASYNC_HTTP_DEBUG("err | not http/1.1 or http/1.0 c=%c\n", p[0]);
                                client->close(true);
                                return;
                            }

                            p += 2; // ' '
                            char buf[4] = {
                                p[0], p[1], p[2], '\0'
                            };
                            ASYNC_HTTP_DEBUG("log | buf code=%s\n", buf);

                            unsigned int code = atoi(buf);
                            if (http->on_status && !http->on_status(http, code)) {
                                ASYNC_HTTP_DEBUG("cb err | http code=%u\n", code);
                                return;
                            }

                            http->state = AsyncHttp::state_t::HEADERS;
                            continue;
                        }
                    case AsyncHttp::state_t::HEADERS:
                        {
                            // TODO: for now, simply skip all headers and go directly to the body
                            p = strnstr(reinterpret_cast<const char *>(response), "\r\n\r\n", len);
                            if (!p) {
                                ASYNC_HTTP_DEBUG("wait | headers not in first %u...\n", len);
                                return;
                            }
                            ASYNC_HTTP_DEBUG("ok | p=%p response=%p diff=%u len=%u\n",
                                p, response, (p - ((char*)response)), len
                            );
                            size_t end = p - ((char*)response) + 4;
                            if (len - end > len) {
                                client->close(true);
                                return;
                            }
                            response = ((char*)response) + end;
                            len -= end;
                            http->state = AsyncHttp::state_t::BODY;
                        }
                    case AsyncHttp::state_t::BODY:
                        {
                            if (!len) {
                                ASYNC_HTTP_DEBUG("wait | len is 0\n");
                                return;
                            }
                            ASYNC_HTTP_DEBUG("ok | body len %u!\n", len);

                            if (http->on_body) http->on_body(http, (uint8_t*) response, len);
                            return;
                        }
                }

            } while (http->state != AsyncHttp::state_t::NONE);

        }

        static void _onConnect(void* http_ptr, AsyncClient * client) {
            AsyncHttp* http = static_cast<AsyncHttp*>(http_ptr);

            http->ts = millis();
            http->connected = true;
            http->connecting = false;

            if (http->on_connected) http->on_connected(http);

            const int headers_len =
                strlen_P(HTTP_REQUEST_TEMPLATE)
                + http->method.length()
                + http->host.length()
                + http->path.length()
                + 32;
            char* headers = (char *) malloc(headers_len + 1);

            if (!headers) {
                ASYNC_HTTP_DEBUG("err | alloc %u fail\n", headers_len + 1);
                client->close(true);
                return;
            }

            int res = snprintf_P(headers, headers_len + 1,
                HTTP_REQUEST_TEMPLATE,
                http->method.c_str(),
                http->path.c_str(),
                http->host.c_str(),
                http->data.length()
            );
            if (res >= (headers_len + 1)) {
                ASYNC_HTTP_DEBUG("err | res>=len :: %u>=%u\n", res, headers_len + 1);
                free(headers);
                client->close(true);
                return;
            }

            client->write(headers);
            free(headers);
            // TODO: streaming data source instead of using a simple String
            // TODO: move to onPoll, ->add(data) and ->send() until it can't (returns 0), then repeat
            client->write(http->data.c_str());

        }

        static void _onError(void* http_ptr, AsyncClient* client, err_t err) {
            AsyncHttp* http = static_cast<AsyncHttp*>(http_ptr);
            if (http->on_error) http->on_error(http, {AsyncHttpError::CLIENT_ERROR, client->errorToString(err)});
        }

    public:
        AsyncHttp() {
            client.onDisconnect(_onDisconnect, this);
            client.onTimeout(_onTimeout, this);
            client.onPoll(_onPoll, this);
            client.onData(_onData, this);
            client.onConnect(_onConnect, this);
            client.onError(_onError, this);
        }
        ~AsyncHttp() = default;

        bool busy() {
            return connecting || connected;
        }

        bool connect(const char* method, const char* host, uint16_t port, const char* path, bool use_ssl = false) {

            this->method = method;
            this->host = host;
            this->port = port;
            this->path = path;
            this->ts = millis();

            bool status = false;

            #if ASYNC_TCP_SSL_ENABLED
                status = client.connect(this->host.c_str(), this->port, use_ssl);
            #else
                status = client.connect(this->host.c_str(), this->port);
            #endif

            this->connecting = status;
            
            if (!status) {
                client.close(true);
            }

            return status;
        }

};
