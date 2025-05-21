#include <chrono>
#include <thread>
#include <rtc/rtc.hpp>
#include "dispatchqueue.hpp"
#include "helpers.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <functional>
#include <memory>
#include <utility>

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
typedef int SOCKET;
#endif

using json = nlohmann::json;

std::string localId = "alice";
const std::string defaultIPAddress = "127.0.0.1";
const uint16_t defaultPort = 8000;
std::string ip_address = defaultIPAddress;
uint16_t port = defaultPort;

template <class T>
std::weak_ptr<T> make_weak_ptr(std::shared_ptr<T> ptr) { return std::weak_ptr<T>(ptr); }


/// all connected clients
std::unordered_map<std::string, std::shared_ptr<Client>> clients{};

/// Creates peer connection and client representation
/// @param config Configuration
/// @param wws Websocket for signaling
/// @param id Client ID
/// @returns Client
std::shared_ptr<Client> createPeerConnection(const rtc::Configuration &config,
                                             std::weak_ptr<rtc::WebSocket> wws,
                                             std::string id);

/// Incomming message handler for websocket
/// @param message Incommint message
/// @param config Configuration
/// @param ws Websocket
void wsOnMessage(json message, rtc::Configuration config, std::shared_ptr<rtc::WebSocket> ws)
{
    auto it = message.find("id");
    if (it == message.end())
        return;
    std::string id = it->get<std::string>();

    it = message.find("type");
    if (it == message.end())
        return;
    std::string type = it->get<std::string>();

    if (type == "offer")
    {
        std::cout << "[Alice] Offer recebido de Bob!" << std::endl;
        std::string sdp = message["sdp"].get<std::string>();
        rtc::Description offer(sdp, type);

        // Cria a conexão sem iniciar oferta
        auto client = createPeerConnection(config, make_weak_ptr(ws), id);
        auto pc = client->peerConnection;

        clients.emplace(id, client);

        pc->setRemoteDescription(offer);
        auto answer = pc->localDescription();
        if (!answer || answer->type() != rtc::Description::Type::Answer) {
            pc->setLocalDescription(rtc::Description::Type::Answer);
        }

        std::cout << "[Alice] Signaling state antes de setLocalDescription: " << static_cast<int>(pc->signalingState()) << std::endl;
    }
    else if (type == "answer")
    {
        if (auto jt = clients.find(id); jt != clients.end())
        {
            auto pc = jt->second->peerConnection;
            auto sdp = message["sdp"].get<std::string>();
            auto description = rtc::Description(sdp, type);
            pc->setRemoteDescription(description);
        }
    }
}

DispatchQueue MainThread("Main");

int main()
{

    rtc::InitLogger(rtc::LogLevel::Debug);

    rtc::Configuration config;
    std::string stunServer = "stun:stun.l.google.com:19302";
    std::cout << "STUN server is " << stunServer << std::endl;
    config.iceServers.emplace_back(stunServer);
    config.disableAutoNegotiation = true;

    auto ws = std::make_shared<rtc::WebSocket>();

    ws->onOpen([]()
               { std::cout << "WebSocket connected, signaling ready" << std::endl; });

    ws->onClosed([]()
                 { std::cout << "WebSocket closed" << std::endl; });

    ws->onError([](const std::string &error)
                { std::cout << "WebSocket failed: " << error << std::endl; });

    ws->onMessage([&](std::variant<rtc::binary, std::string> data)
                  {
        if (!std::holds_alternative<std::string>(data))
            return;

        json message = json::parse(std::get<std::string>(data));
        MainThread.dispatch([message, config, ws]() {
            wsOnMessage(message, config, ws);
        }); });

    const std::string url = "ws://" + ip_address + ":" + std::to_string(port) + "/" + localId;
    std::cout << "We'll connect to " << url << " as " << localId << std::endl;
    std::cout << "Waiting for signaling to be connected..." << std::endl;
    ws->open(url);

    while (!ws->isOpen())
    {
        if (ws->isClosed())
            return 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    while (true)
    {
        std::string id;
        std::cout << "Enter to exit" << std::endl;
        std::cin >> id;
        std::cin.ignore();
        std::cout << "exiting" << std::endl;
        break;
    }

    std::cout << "Cleaning up..." << std::endl;
    return 0;
}

std::shared_ptr<Client> createPeerConnection(const rtc::Configuration &config,
                                             std::weak_ptr<rtc::WebSocket> wws,
                                             std::string id)
{
    auto pc = std::make_shared<rtc::PeerConnection>(config);

    auto client = std::make_shared<Client>(pc);

    pc->onTrack([client](std::shared_ptr<rtc::Track> track) {
        std::cout << "[Alice] Track recebida! Mid: " << track->mid()
                  << ", Tipo: " << track->description().type() << std::endl;

        client->remoteTracks.push_back(track); // <-- Guarde a referência

        track->onMessage([](rtc::message_variant msg) {
            if (std::holds_alternative<rtc::binary>(msg)) {
                auto data = std::get<rtc::binary>(msg);
                std::cout << "[Alice] Pacote recebido (" << data.size() << " bytes)" << std::endl;
                std::cout << "[Alice] Pacote: ";
                for (auto byte : data) {
                    std::cout << std::hex << static_cast<int>(byte) << " ";
                }
                std::cout << std::dec << std::endl;
            }
        });
    });

    pc->onStateChange([id](rtc::PeerConnection::State state)
    {
        std::cout << "State: " << state << std::endl;
        if (state == rtc::PeerConnection::State::Disconnected ||
            state == rtc::PeerConnection::State::Failed ||
            state == rtc::PeerConnection::State::Closed)
        {
            MainThread.dispatch([id]() {
                clients.erase(id);
            });
        }
    });

    pc->onGatheringStateChange([wpc = make_weak_ptr(pc), id, wws](rtc::PeerConnection::GatheringState state)
    {
        std::cout << "Gathering State: " << state << std::endl;
        if (state == rtc::PeerConnection::GatheringState::Complete)
        {
            if (auto pc = wpc.lock())
            {
                auto description = pc->localDescription();
                json message = {
                    {"id", id},
                    {"type", description->typeString()},
                    {"sdp", std::string(description.value())}};
                if (auto ws = wws.lock())
                {
                    ws->send(message.dump());
                }
                std::cout << "[Alice] Enviando SDP tipo: " << description->typeString() << std::endl;
                std::cout << "[Alice] SDP enviado:\n" << std::string(description.value()) << std::endl;
            }
        }
    });

    return client;
}