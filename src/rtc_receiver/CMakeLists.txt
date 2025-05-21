project(RTC_Receiver)

set(STREAMER_SOURCES
	main.cpp
	dispatchqueue.cpp
	dispatchqueue.hpp
	helpers.cpp
	helpers.hpp
)

find_package(Threads REQUIRED)

# Caso tenha instalado com CMake
find_package(ixwebsocket REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(LibDataChannel REQUIRED CONFIG PATHS /usr/lib/cmake/LibDataChannel)

add_executable(RTC_Receiver
    main.cpp
    ${STREAMER_SOURCES}
)

target_link_libraries(RTC_Receiver
    ixwebsocket::ixwebsocket
    nlohmann_json::nlohmann_json
    Threads::Threads
    LibDataChannel::LibDataChannel
)
