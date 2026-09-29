/**
 * Device login shared by every game on this machine
 *
 * Desktop builds keep the device token in one file per site, in a folder all
 * A.D.S. Games games use, so a device linked in one game is logged in in all
 * of them:
 *
 *   macOS    ~/Library/Application Support/adsgames/play/
 *   Windows  %APPDATA%\adsgames\play\
 *   Linux    $XDG_CONFIG_HOME/adsgames/play/ (or ~/.config)
 *
 * ADSGAMES_PLAY_LOGIN_DIR overrides the folder. Web builds have no shared
 * folder, they use the site's cookie instead.
 */

#pragma once

#include <string>

namespace adsgames::play::login_store {

// File for a site, e.g. .../login-www.adsgames.net.json. Empty when there is
// no shared folder, or the site URL is relative.
std::string path_for(const std::string& site_url);

// File in the shared folder named prefix-<site host>-suffix.json, e.g. a
// game's saved stats. Empty like path_for().
std::string file_for(
    const std::string& site_url, const std::string& prefix, const std::string& suffix);

// The saved device token, "" if there is none
std::string load(const std::string& path);

// Saves a device token, "" removes the file. Failures are ignored, the game
// then stays logged in until it exits.
void save(const std::string& path, const std::string& token);

// Reads a whole file, "" if it is missing
std::string read_text(const std::string& path);

// Writes a file only this user can read, swapping it in so readers never see
// half of it. Failures are ignored.
void write_text(const std::string& path, const std::string& text);

} // namespace adsgames::play::login_store
