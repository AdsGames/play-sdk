// Default URLs for each platform

#include <array>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string>

#include "adsgames/play/client.h"
#include "transport.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#elif !defined(_WIN32)
#include <unistd.h>
#endif

namespace adsgames::play {

std::string default_base_url()
{
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
    if (const char* env = std::getenv("ADSGAMES_PLAY_URL"); env != nullptr && *env != '\0') {
        return env;
    }
    return ADSGAMES_PLAY_URL;
#endif
}

std::string default_site_url(const std::string& base_url)
{
    // play sits under /api/play on the site, see the load balancer config
    const std::string suffix = "/api/play";
    if (base_url.ends_with(suffix)) {
        return base_url.substr(0, base_url.size() - suffix.size());
    }
    return ADSGAMES_SITE_URL;
}

std::string default_device_name()
{
#if defined(__EMSCRIPTEN__)
    return "Web browser";
#else
    std::string host;
#if defined(_WIN32)
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    if (const char* name = std::getenv("COMPUTERNAME"); name != nullptr) {
        host = name;
    }
    const std::string os = "Windows";
#else
    std::array<char, 256> name { };
    if (gethostname(name.data(), name.size() - 1) == 0) {
        host = name.data();
    }
    // macOS names end in .local
    if (const auto dot = host.find('.'); dot != std::string::npos) {
        host.resize(dot);
    }
#if defined(__APPLE__)
    const std::string os = "macOS";
#else
    const std::string os = "Linux";
#endif
#endif
    return host.empty() ? os : host + " (" + os + ")";
#endif
}

std::string multiplayer_url_for(const std::string& base_url)
{
    std::string url = base_url + "/multiplayer";

    if (url.starts_with("https://")) {
        return "wss://" + url.substr(8);
    }
    if (url.starts_with("http://")) {
        return "ws://" + url.substr(7);
    }

#if defined(__EMSCRIPTEN__)
    // A path like /api/play, resolve it against the page. The json dump is a
    // valid JavaScript string literal.
    const std::string script = "(function(){try{var u=new URL("
        + nlohmann::json(url).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)
        + ",window.location.href);u.protocol=u.protocol==='https:'?'wss:':'ws:';"
          "return u.toString();}catch(e){return '';}})()";
    const char* resolved = emscripten_run_script_string(script.c_str());
    return resolved != nullptr ? resolved : "";
#else
    return url;
#endif
}

} // namespace adsgames::play
