#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include "dispatchqueue.hpp"
#include "helpers.hpp"
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>

typedef int SOCKET;

using json = nlohmann::json;

const int BUFFER_SIZE = 8192; // Good size to receive ethernet packages

std::atomic<bool> running{true};

std::string localId = "bob";
const std::string defaultIPAddress = "192.168.0.114";
const uint16_t defaultPort = 8000;
std::string ip_address = defaultIPAddress;
uint16_t port = defaultPort;

SOCKET video_socket = socket(AF_INET, SOCK_DGRAM, 0);
SOCKET audio_socket = socket(AF_INET, SOCK_DGRAM, 0);

/// Para guardar o WebSocket
std::unordered_map<std::string, std::shared_ptr<Client>> clients{};

DispatchQueue MainThread("Main");

template <class T>
std::weak_ptr<T> make_weak_ptr(std::shared_ptr<T> ptr) { return std::weak_ptr<T>(ptr); }

void receiveVideo(SOCKET sock, std::shared_ptr<rtc::Track> videoTrack, rtc::SSRC ssrcVideo) {
    char videoBuffer[BUFFER_SIZE];
    uint32_t packetCount = 0;

    while (running) {
        int videoLen = recv(sock, videoBuffer, BUFFER_SIZE, 0);

        if (videoLen <= sizeof(rtc::RtpHeader)) {
            std::cerr << "[Bob] Pacote de vídeo muito pequeno ou inválido" << std::endl;
            continue;
        }

        if (!videoTrack->isOpen()) {
            std::cerr << "[Bob] Track de vídeo não está aberta" << std::endl;
            continue;
        }

        auto videoRtp = reinterpret_cast<rtc::RtpHeader*>(videoBuffer);
        videoRtp->setSsrc(ssrcVideo);

        // Log a cada 100 pacotes para evitar spam
        if (packetCount++ % 100 == 0) {
            std::cout << "[Bob] Enviando pacote de vídeo (SSRC: " << ssrcVideo 
                      << ", tamanho: " << videoLen << " bytes)" << std::endl;
        }

        videoTrack->send(reinterpret_cast<const std::byte*>(videoBuffer), videoLen);
    }
}
void receiveAudio(SOCKET sock, std::shared_ptr<rtc::Track> audioTrack, rtc::SSRC ssrcAudio) {
    char audioBuffer[BUFFER_SIZE];
    uint32_t packetCount = 0;

    while (running) {
        int audioLen = recv(sock, audioBuffer, BUFFER_SIZE, 0);

        if (audioLen <= sizeof(rtc::RtpHeader)) {
            std::cerr << "[Bob] Pacote de áudio muito pequeno ou inválido" << std::endl;
            continue;
        }

        if (!audioTrack->isOpen()) {
            std::cerr << "[Bob] Track de áudio não está aberta" << std::endl;
            continue;
        }

        auto audioRtp = reinterpret_cast<rtc::RtpHeader*>(audioBuffer);
        audioRtp->setSsrc(ssrcAudio);

        if (packetCount++ % 100 == 0) {
            std::cout << "[Bob] Enviando pacote de áudio (SSRC: " << ssrcAudio 
                      << ", tamanho: " << audioLen << " bytes)" << std::endl;
        }

        audioTrack->send(reinterpret_cast<const std::byte*>(audioBuffer), audioLen);
    }
}


std::shared_ptr<Client> createPeerConnection(const rtc::Configuration &config,
                                             std::weak_ptr<rtc::WebSocket> wws,
                                             std::string remoteId)
{
    auto pc = std::make_shared<rtc::PeerConnection>(config);

    // Adicionar áudio e vídeo (sendonly)
    const rtc::SSRC video_ssrc = 43;
    rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
    video.addH264Codec(96);
    video.addSSRC(video_ssrc, "video-send");
    auto video_track = pc->addTrack(video);

    const rtc::SSRC audio_ssrc = 44;
    rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);
    audio.addOpusCodec(97);
    audio.addSSRC(audio_ssrc, "audio-send");
    auto audio_track = pc->addTrack(audio);

    pc->setLocalDescription(); // <-- Necessário para disparar a oferta e o ICE gathering

    auto client = std::make_shared<Client>(pc);

    struct sockaddr_in videoAddr = {};
    videoAddr.sin_family = AF_INET;
    videoAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    videoAddr.sin_port = htons(6000);
    
    struct sockaddr_in audioAddr = {};
    audioAddr.sin_family = AF_INET;
    audioAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    audioAddr.sin_port = htons(6001);

    int VideoRcvBufSize = 512*1024;
    int AudioRcvBufSize = 128*1024;
    setsockopt(video_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&VideoRcvBufSize), sizeof(VideoRcvBufSize));
    setsockopt(audio_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&AudioRcvBufSize), sizeof(AudioRcvBufSize));


    if (bind(video_socket, reinterpret_cast<const sockaddr *>(&videoAddr), sizeof(videoAddr)) < 0) {
        throw std::runtime_error("Failed to bind the UDP video socket, port 6000");
    }

    if (bind(audio_socket, reinterpret_cast<const sockaddr *>(&audioAddr), sizeof(audioAddr)) < 0) {
        throw std::runtime_error("Failed to bind the UDP audio socket, port 6001");
    }

    pc->onStateChange([remoteId](rtc::PeerConnection::State state)
                      {
        std::cout << "State: " << state << std::endl;
        if (state == rtc::PeerConnection::State::Disconnected ||
            state == rtc::PeerConnection::State::Failed ||
            state == rtc::PeerConnection::State::Closed)
        {
            MainThread.dispatch([remoteId]() {
                clients.erase(remoteId);
            });
        } });

    pc->onGatheringStateChange(
        [wpc = make_weak_ptr(pc), remoteId, wws](rtc::PeerConnection::GatheringState state)
        {
            std::cout << "Gathering State: " << state << std::endl;
            if (state == rtc::PeerConnection::GatheringState::Complete)
            {
                if (auto pc = wpc.lock())
                {
                    auto description = pc->localDescription();
                    if (auto ws = wws.lock())
                    {
                        json offer = {
                            {"id", "alice"},
                            {"type", description->typeString()},
                            {"sdp", std::string(description.value())}
                        };
                        ws->send(offer.dump());
                        std::cout << "[Bob] Offer enviada para Alice!" << std::endl;
                    }
                }
            }
        });

        std::thread videoThread(receiveVideo, video_socket, video_track, video_ssrc);
        std::thread audioThread(receiveAudio, audio_socket, audio_track, audio_ssrc);
        
        videoThread.detach();
        audioThread.detach();

    return client;
}

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

    if (type == "answer")
    {
        if (auto jt = clients.find(id); jt != clients.end())
        {
            auto pc = jt->second->peerConnection;
            std::string sdp = message["sdp"].get<std::string>();
            rtc::Description answer(sdp, type);
            pc->setRemoteDescription(answer);
            std::cout << "Answer received from Alice!" << std::endl;
        }
    }
}

int main()
{
    rtc::InitLogger(rtc::LogLevel::Debug);

    rtc::Configuration config;
    std::string stunServer = "stun:stun.l.google.com:19302";
    std::cout << "STUN server is " << stunServer << std::endl;
    config.iceServers.emplace_back(stunServer);
    config.disableAutoNegotiation = false; // Bob deixa a auto-negociação ligada

    auto ws = std::make_shared<rtc::WebSocket>();

    ws->onOpen([&]()
               {
                   std::cout << "WebSocket connected!" << std::endl;
                   // Cria peer connection e envia oferta
                   auto client = createPeerConnection(config, make_weak_ptr(ws), "alice");
                   clients.emplace("alice", client);
               });

    ws->onClosed([]()
                 { std::cout << "WebSocket closed!" << std::endl; });

    ws->onError([](const std::string &error)
                { std::cout << "WebSocket error: " << error << std::endl; });

    ws->onMessage([&](std::variant<rtc::binary, std::string> data)
                  {
        if (!std::holds_alternative<std::string>(data))
            return;

        json message = json::parse(std::get<std::string>(data));
        MainThread.dispatch([message, config, ws]() {
            wsOnMessage(message, config, ws);
        }); });

    std::string url = "ws://" + ip_address + ":" + std::to_string(port) + "/" + localId;
    ws->open(url);

    while (!ws->isOpen())
    {
        if (ws->isClosed())
            return 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    while (true)
    {
        std::string dummy;
        std::cout << "Press enter to exit..." << std::endl;
        std::getline(std::cin, dummy);
        break;
    }

    close(video_socket);
    close(audio_socket);

    return 0;
}