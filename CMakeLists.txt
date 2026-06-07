cmake_minimum_required(VERSION 3.20)
project(scrapeff VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Gumbo)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Widgets Concurrent)

# Qt forwarding headers include as <QtWidgets/…>; compiler needs prefix/include, not only …/include/QtWidgets.
get_filename_component(QT6_BASE_INCLUDE "${Qt6_DIR}/../../../include" ABSOLUTE)

add_library(scrapeff_core STATIC
  src/dom_helpers.cpp
  src/phash.cpp
  src/html_parse.cpp
  src/url_parse.cpp
  src/ssl_utils.cpp
  src/http_request.cpp
)
target_include_directories(scrapeff_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(scrapeff_core PUBLIC Qt6::Core Qt6::Gui Qt6::Network scrapeff_gumbo)

add_executable(scrapeff src/main_scrape.cpp src/scrape_runner.cpp src/cli_scrape.cpp)
target_link_libraries(scrapeff PRIVATE scrapeff_core Qt6::Concurrent)

add_executable(scrape_requests src/main_scrape_requests.cpp src/scrape_runner.cpp src/cli_scrape.cpp)
target_link_libraries(scrape_requests PRIVATE scrapeff_core Qt6::Concurrent)


add_executable(gallery_qt
  src/gallery/main.cpp
  src/gallery/mainwindow.cpp
  src/scrape_runner.cpp
  src/cli_scrape.cpp
)
target_include_directories(gallery_qt PRIVATE "${QT6_BASE_INCLUDE}")
target_link_libraries(gallery_qt PRIVATE scrapeff_core Qt6::Widgets Qt6::Network Qt6::Concurrent)
