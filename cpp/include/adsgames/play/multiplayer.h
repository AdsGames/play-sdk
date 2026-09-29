/**
 * Connection to the play multiplayer relay
 *
 * The relay pairs two players in a room named by a 4 character code and
 * forwards game messages between them. It does not read game messages, so
 * each game checks the other player's moves itself.
 */

#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace adsgames::play {

class WebSocketClient;

class Multiplayer {
 public:
  enum class State : uint8_t {
    // Opening the socket
    Connecting,

    // In a room, waiting for the other player
    Waiting,

    // Both players are in the room
    Paired,

    // The room or socket closed, see error() and peer_left()
    Closed,
  };

  // Connect and open a new room. Players with a different game or version
  // can not share a room.
  static std::shared_ptr<Multiplayer> host(const std::string& url,
                                           const std::string& game,
                                           int version);

  // Connect and join the room with the given code, case insensitive
  static std::shared_ptr<Multiplayer> join(const std::string& url,
                                           const std::string& game,
                                           int version,
                                           const std::string& code);

  ~Multiplayer();

  Multiplayer(const Multiplayer&) = delete;
  Multiplayer& operator=(const Multiplayer&) = delete;
  Multiplayer(Multiplayer&&) = delete;
  Multiplayer& operator=(Multiplayer&&) = delete;

  // Read network events, call once per frame
  void update();

  // Send a game message to the other player, ignored until paired
  void send(const nlohmann::json& data);

  // Next game message from the other player
  std::optional<nlohmann::json> receive();

  // Leave the room, the other player sees peer_left()
  void leave();

  State state() const { return state_; }

  // Room code, set once the room is created or joined
  const std::string& code() const { return code_; }

  // 0 for the host, 1 for the player who joined, -1 before either
  int seat() const { return seat_; }

  // Other player's adsgames.net username, empty for guests
  const std::string& peer_name() const { return peer_name_; }

  // Set when the other player left after being paired
  bool peer_left() const { return peer_left_; }

  // Last error message, empty when there was none
  const std::string& error() const { return error_; }

  // Server error code (room_not_found, version_mismatch, …) or "connection"
  const std::string& error_code() const { return error_code_; }

  // False when built without network support
  static bool available();

 private:
  Multiplayer(std::unique_ptr<WebSocketClient> socket, nlohmann::json hello);

  static std::shared_ptr<Multiplayer> open(const std::string& url,
                                           nlohmann::json hello);

  void handle_message(const std::string& text);

  void close(const std::string& code, const std::string& message);

  std::unique_ptr<WebSocketClient> socket_;
  nlohmann::json hello_;
  State state_{State::Connecting};
  bool peer_left_{false};
  int seat_{-1};
  std::string code_;
  std::string peer_name_;
  std::string error_;
  std::string error_code_;
  std::deque<nlohmann::json> inbox_;
};

}  // namespace adsgames::play
