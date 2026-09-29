// Default URLs for each platform

#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string>

#include "adsgames/play/client.h"
#include "transport.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

namespace adsgames::play {

namespace {

bool starts_with(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

}  // namespace

std::string default_base_url() {
#if defined(__EMSCRIPTEN__)
  // ?play= on the page URL wins so players can point at a local server
  const char* param = emscripten_run_script_string(
      "(function(){try{return new URLSearchParams(window.location.search)"
      ".get('play')||'';}catch(e){return '';}})()");
  std::string url = param != nullptr ? param : "";

  // Same-origin on adsgames.net, so requests send the session cookie
  return url.empty() ? "/api/play" : url;
#else
  // Environment variable wins so players can point at a local server
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  if (const char* env = std::getenv("ADSGAMES_PLAY_URL");
      env != nullptr && *env != '\0') {
    return env;
  }
  return ADSGAMES_PLAY_URL;
#endif
}

std::string multiplayer_url_for(const std::string& base_url) {
  const std::string url = base_url + "/multiplayer";

  if (starts_with(url, "https://")) {
    return "wss://" + url.substr(8);
  }
  if (starts_with(url, "http://")) {
    return "ws://" + url.substr(7);
  }

#if defined(__EMSCRIPTEN__)
  // A path like /api/play, resolve it against the page. The json dump is a
  // valid JavaScript string literal.
  const std::string script =
      "(function(){try{var u=new URL(" +
      nlohmann::json(url).dump(-1, ' ', false,
                               nlohmann::json::error_handler_t::replace) +
      ",window.location.href);u.protocol=u.protocol==='https:'?'wss:':'ws:';"
      "return u.toString();}catch(e){return '';}})()";
  const char* resolved = emscripten_run_script_string(script.c_str());
  return resolved != nullptr ? resolved : "";
#else
  return url;
#endif
}

}  // namespace adsgames::play
