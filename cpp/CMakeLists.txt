cmake_minimum_required(VERSION 3.24)

project(adsgames_play VERSION 0.1.0 LANGUAGES CXX)

option(ADSGAMES_PLAY_NETWORK "Build network support, off gives a client whose requests all fail" ON)
option(ADSGAMES_PLAY_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})

# Desktop builds have no site cookie, so they call play on www.adsgames.net.
# Players can override it with the ADSGAMES_PLAY_URL environment variable.
# Web builds use /api/play on the page's own host, or ?play= on the page URL.
set(ADSGAMES_PLAY_URL "https://www.adsgames.net/api/play"
    CACHE STRING "Default play URL for desktop builds")

include(FetchContent)

# Games often have json already, for example through CPM
if(NOT TARGET nlohmann_json::nlohmann_json)
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(adsgames_play STATIC
  src/client.cpp
  src/http_native.cpp
  src/http_web.cpp
  src/json_read.cpp
  src/multiplayer.cpp
  src/platform.cpp
  src/websocket_native.cpp
  src/websocket_web.cpp
)
add_library(adsgames::play ALIAS adsgames_play)

target_include_directories(adsgames_play PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(adsgames_play PUBLIC cxx_std_20)
target_compile_options(adsgames_play PRIVATE -Wall -Wextra -pedantic)
target_compile_definitions(adsgames_play PRIVATE ADSGAMES_PLAY_URL="${ADSGAMES_PLAY_URL}")
target_link_libraries(adsgames_play PUBLIC nlohmann_json::nlohmann_json)

if(ADSGAMES_PLAY_NETWORK)
  target_compile_definitions(adsgames_play PUBLIC ADSGAMES_PLAY_NETWORK=1)

  if(EMSCRIPTEN)
    # The browser has fetch and WebSockets built in
    target_link_options(adsgames_play PUBLIC -lwebsocket.js -sFETCH=1)
  else()
    if(NOT TARGET ixwebsocket::ixwebsocket)
      # TLS backend: Secure Transport on macOS (IXWebSocket default), OpenSSL
      # elsewhere. MinGW links everything static, so link OpenSSL static too.
      set(USE_TLS ON CACHE INTERNAL "")
      set(USE_ZLIB OFF CACHE INTERNAL "")
      set(IXWEBSOCKET_INSTALL OFF CACHE INTERNAL "")
      if(WIN32)
        set(USE_OPEN_SSL ON CACHE INTERNAL "")
        set(OPENSSL_USE_STATIC_LIBS ON)
      endif()

      FetchContent_Declare(
        ixwebsocket
        URL https://github.com/machinezone/IXWebSocket/archive/refs/tags/v12.0.1.tar.gz
      )
      FetchContent_MakeAvailable(ixwebsocket)
    endif()

    target_link_libraries(adsgames_play PUBLIC ixwebsocket::ixwebsocket)
  endif()
endif()

# Tests run against a play server, see README.md
if(ADSGAMES_PLAY_BUILD_TESTS AND NOT EMSCRIPTEN)
  enable_testing()

  add_executable(play_test tests/play_test.cpp)
  target_link_libraries(play_test PRIVATE adsgames::play)
  add_test(NAME play_test COMMAND play_test)
  set_tests_properties(play_test PROPERTIES SKIP_RETURN_CODE 77)
endif()
