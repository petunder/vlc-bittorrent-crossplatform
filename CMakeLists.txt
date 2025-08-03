cmake_minimum_required(VERSION 3.14)

project(
  vlc-plugin-bittorrent
  VERSION 3.0
  DESCRIPTION "A BitTorrent input plugin for VLC"
  HOMEPAGE_URL "https://github.com/petunder/vlc-bittorrent-crossplatform"
  LANGUAGES C CXX
)

# На Windows: требуем, чтобы через -DCMAKE_PREFIX_PATH был передан путь к распакованному VLC SDK
if (WIN32)
  if (NOT DEFINED CMAKE_PREFIX_PATH)
    message(FATAL_ERROR
      "On Windows please set CMAKE_PREFIX_PATH to your VLC SDK root, e.g.\n"
      "  -DCMAKE_PREFIX_PATH=C:/path/to/vlc-3.0.21-win64/sdk"
    )
  endif()
endif()

add_subdirectory(src)
