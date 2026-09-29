#include "login_store.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>

namespace adsgames::play::login_store {

namespace {

    namespace fs = std::filesystem;

    std::string env(const char* name)
    {
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        const char* value = std::getenv(name);
        return value != nullptr ? value : "";
    }

    fs::path shared_folder()
    {
        if (auto dir = env("ADSGAMES_PLAY_LOGIN_DIR"); !dir.empty()) {
            return dir;
        }

#if defined(__EMSCRIPTEN__)
        return { };
#elif defined(_WIN32)
        const auto appdata = env("APPDATA");
        return appdata.empty() ? fs::path { } : fs::path(appdata) / "adsgames" / "play";
#elif defined(__APPLE__)
        const auto home = env("HOME");
        return home.empty()
            ? fs::path { }
            : fs::path(home) / "Library" / "Application Support" / "adsgames" / "play";
#else
        if (const auto config = env("XDG_CONFIG_HOME"); !config.empty()) {
            return fs::path(config) / "adsgames" / "play";
        }
        const auto home = env("HOME");
        return home.empty() ? fs::path { } : fs::path(home) / ".config" / "adsgames" / "play";
#endif
    }

    // "https://www.adsgames.net:443/x" gives "www.adsgames.net_443"
    std::string host_of(const std::string& site_url)
    {
        const auto scheme = site_url.find("://");
        if (scheme == std::string::npos) {
            return "";
        }

        const auto start = scheme + 3;
        const auto end = site_url.find('/', start);
        const auto host
            = site_url.substr(start, end == std::string::npos ? std::string::npos : end - start);

        std::string out;
        for (const unsigned char c : host) {
            out += (std::isalnum(c) != 0 || c == '.' || c == '-')
                ? static_cast<char>(std::tolower(c))
                : '_';
        }
        return out;
    }

} // namespace

std::string path_for(const std::string& site_url)
{
    return file_for(site_url, "login", "");
}

std::string file_for(
    const std::string& site_url, const std::string& prefix, const std::string& suffix)
{
    const auto folder = shared_folder();
    const auto host = host_of(site_url);
    if (folder.empty() || host.empty()) {
        return "";
    }
    auto name = prefix + "-" + host;
    if (!suffix.empty()) {
        name += "-" + suffix;
    }
    return (folder / (name + ".json")).string();
}

std::string read_text(const std::string& path)
{
    if (path.empty()) {
        return "";
    }
    const std::ifstream file(path);
    if (!file) {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string load(const std::string& path)
{
    const auto j = nlohmann::json::parse(read_text(path), nullptr, false);
    if (!j.is_object()) {
        return "";
    }
    const auto it = j.find("deviceToken");
    return it != j.end() && it->is_string() ? it->get<std::string>() : "";
}

void save(const std::string& path, const std::string& token)
{
    if (path.empty()) {
        return;
    }

    if (token.empty()) {
        std::error_code ec;
        fs::remove(path, ec);
        return;
    }

    write_text(path, nlohmann::json { { "deviceToken", token } }.dump());
}

void write_text(const std::string& path, const std::string& text)
{
    if (path.empty()) {
        return;
    }

    std::error_code ec;
    const fs::path target(path);
    fs::create_directories(target.parent_path(), ec);

    // Write a temporary file and swap it in, so a game reading the file never
    // sees half a token
    const fs::path temp = target.string() + ".tmp";
    {
        const std::ofstream file(temp, std::ios::trunc);
        if (!file) {
            return;
        }
    }
    // Only this user can read the token
    fs::permissions(
        temp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    {
        std::ofstream file(temp, std::ios::trunc);
        file << text;
        if (!file) {
            fs::remove(temp, ec);
            return;
        }
    }

    fs::rename(temp, target, ec);
    if (ec) {
        // Some platforms do not replace an existing file on rename
        fs::remove(target, ec);
        fs::rename(temp, target, ec);
    }
}

} // namespace adsgames::play::login_store
