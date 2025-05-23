cmake_minimum_required(VERSION 3.10)
project(WebRTC_Workspace)

# Configurações comuns para todos os projetos
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(src/rtc_receiver)
add_subdirectory(src/rtc_sender)