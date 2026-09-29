#include "adsgames/play/multiplayer.h"

#include <cctype>
#include <utility>

#include "transport.h"

namespace adsgames::play {

Multiplayer::Multiplayer(std::unique_ptr<WebSocketClient> socket, nlohmann::json hello)
    : socket_(std::move(socket))
    , hello_(std::move(hello))
{
}

Multiplayer::~Multiplayer() = default;

std::shared_ptr<Multiplayer> Multiplayer::host(
    const std::string& url, const std::string& game, int version)
{
    return open(url, { { "type", "create" }, { "game", game }, { "version", version } });
}

std::shared_ptr<Multiplayer> Multiplayer::join(
    const std::string& url, const std::string& game, int version, const std::string& code)
{
    std::string upper = code;
    for (auto& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    auto session = open(
        url, { { "type", "join" }, { "game", game }, { "version", version }, { "code", upper } });
    session->code_ = upper;
    return session;
}

std::shared_ptr<Multiplayer> Multiplayer::open(const std::string& url, nlohmann::json hello)
{
    auto session = std::shared_ptr<Multiplayer>(
        new Multiplayer(WebSocketClient::create(), std::move(hello)));

    if (!session->socket_) {
        session->close("connection", "Online play is not in this build");
        return session;
    }

    session->socket_->connect(url);
    return session;
}

bool Multiplayer::available()
{
#if defined(ADSGAMES_PLAY_NETWORK)
    return true;
#else
    return false;
#endif
}

void Multiplayer::update()
{
    if (!socket_) {
        return;
    }

    for (const auto& event : socket_->poll()) {
        if (state_ == State::Closed) {
            break;
        }

        switch (event.type) {
        case WebSocketClient::Event::Type::Open:
            socket_->send(hello_.dump());
            break;

        case WebSocketClient::Event::Type::Message:
            handle_message(event.data);
            break;

        case WebSocketClient::Event::Type::Close:
            close("connection",
                event.data.empty() ? "Disconnected from server" : "Disconnected: " + event.data);
            break;

        case WebSocketClient::Event::Type::Error:
            close("connection",
                event.data.empty() ? "Could not reach server"
                                   : "Could not reach server: " + event.data);
            break;
        }
    }
}

void Multiplayer::handle_message(const std::string& text)
{
    // Anything the server sends is untrusted input
    const auto message = nlohmann::json::parse(text, nullptr, false);
    if (message.is_discarded() || !message.is_object() || !message.contains("type")
        || !message["type"].is_string()) {
        return;
    }

    const auto type = message["type"].get<std::string>();

    const auto read_string = [&message](const char* field) {
        if (message.contains(field) && message[field].is_string()) {
            return message[field].get<std::string>();
        }
        return std::string();
    };

    if (type == "created" || type == "joined") {
        code_ = read_string("code");
        if (message.contains("seat") && message["seat"].is_number_integer()) {
            seat_ = message["seat"].get<int>();
        }
        // Joining a room means the host is already in it
        if (type == "joined") {
            state_ = State::Paired;
            peer_name_ = read_string("peerName");
        } else {
            state_ = State::Waiting;
        }
    } else if (type == "peer_joined") {
        state_ = State::Paired;
        peer_name_ = read_string("name");
        peer_left_ = false;
    } else if (type == "peer_left") {
        // The server closes the room when a player leaves
        peer_left_ = true;
        close("", "");
    } else if (type == "message") {
        if (message.contains("data")) {
            inbox_.push_back(message["data"]);
        }
    } else if (type == "error") {
        error_code_ = read_string("code");
        error_ = read_string("message");
        if (error_.empty()) {
            error_ = "Server error";
        }
        // Without a room there is nothing left to do on this socket
        if (state_ == State::Connecting) {
            close(error_code_, error_);
        }
    }
}

void Multiplayer::send(const nlohmann::json& data)
{
    if (socket_ && state_ == State::Paired) {
        socket_->send(nlohmann::json({ { "type", "send" }, { "data", data } })
                .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    }
}

std::optional<nlohmann::json> Multiplayer::receive()
{
    if (inbox_.empty()) {
        return std::nullopt;
    }

    auto message = std::move(inbox_.front());
    inbox_.pop_front();
    return message;
}

void Multiplayer::leave()
{
    if (state_ == State::Closed) {
        return;
    }
    if (socket_ && state_ != State::Connecting) {
        socket_->send(R"({"type":"leave"})");
    }
    close("", "");
}

void Multiplayer::close(const std::string& code, const std::string& message)
{
    if (state_ == State::Closed) {
        return;
    }

    state_ = State::Closed;
    if (!message.empty()) {
        error_code_ = code;
        error_ = message;
    }

    if (socket_) {
        socket_->close();
    }
}

} // namespace adsgames::play
