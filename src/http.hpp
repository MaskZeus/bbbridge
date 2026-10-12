// HTTP: a WinHTTP client (certificates checked by Windows, no redirects
// followed) and a small threaded HTTP/1.1 server for the bridge's local
// listeners (WebAPI, game-server proxy).
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "util.hpp"

namespace http {

using Headers = std::vector<std::pair<std::string, std::string>>;

std::string header_value(const Headers& headers, const std::string& name);   // case-insensitive, "" if absent

struct ClientRequest {
    std::string method = "POST";
    std::string url;
    Headers headers;
    std::string body;
    int timeout_ms = 8000;
};

struct ClientResponse {
    int status = 0;
    Headers headers;
    std::string body;
    std::string error;          // transport failure (no HTTP answer)
    bool ok() const { return error.empty(); }
};

ClientResponse send(const ClientRequest& req);

struct ServerRequest {
    std::string method;
    std::string target;         // path with the query
    Headers headers;
    std::string body;
    std::string path() const { return target.substr(0, target.find('?')); }
    std::string query() const {
        const size_t q = target.find('?');
        return q == std::string::npos ? "" : target.substr(q + 1);
    }
};

struct ServerResponse {
    int status = 200;
    Headers headers;            // Content-Length is added
    std::string body;
    static ServerResponse json(const bb::json& j, int status = 200);
};

using Handler = std::function<ServerResponse(const ServerRequest&)>;

class Server {
public:
    ~Server() { stop(); }
    bool start(const std::string& host, int port, Handler handler, std::string* error);
    void stop();
    int port() const { return port_; }

private:
    void accept_loop();
    void serve(SOCKET s);

    SOCKET listen_ = INVALID_SOCKET;
    int port_ = 0;
    Handler handler_;
    std::thread acceptor_;
    std::atomic<bool> running_{false};
    std::mutex mu_;
    std::set<SOCKET> clients_;
    std::atomic<int> active_{0};   // detached connection threads still running
};

}  // namespace http
