#include "sora_signaling.h"
#include "sora_version.h"

#include <boost/asio/connect.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/json.hpp>
#include <boost/preprocessor/stringize.hpp>

namespace {

std::string iceConnectionStateToString(
    webrtc::PeerConnectionInterface::IceConnectionState state) {
  switch (state) {
    case webrtc::PeerConnectionInterface::kIceConnectionNew:
      return "new";
    case webrtc::PeerConnectionInterface::kIceConnectionChecking:
      return "checking";
    case webrtc::PeerConnectionInterface::kIceConnectionConnected:
      return "connected";
    case webrtc::PeerConnectionInterface::kIceConnectionCompleted:
      return "completed";
    case webrtc::PeerConnectionInterface::kIceConnectionFailed:
      return "failed";
    case webrtc::PeerConnectionInterface::kIceConnectionDisconnected:
      return "disconnected";
    case webrtc::PeerConnectionInterface::kIceConnectionClosed:
      return "closed";
    case webrtc::PeerConnectionInterface::kIceConnectionMax:
      return "max";
  }
  return "unknown";
}

}  // namespace

namespace sora {

webrtc::PeerConnectionInterface::IceConnectionState
SoraSignaling::getRTCConnectionState() const {
  return rtc_state_;
}

std::shared_ptr<RTCConnection> SoraSignaling::getRTCConnection() const {
  if (rtc_state_ == webrtc::PeerConnectionInterface::IceConnectionState::
                        kIceConnectionConnected) {
    return connection_;
  } else {
    return nullptr;
  }
}

std::shared_ptr<SoraSignaling> SoraSignaling::Create(
    boost::asio::io_context& ioc,
    RTCManager* manager,
    SoraSignalingConfig config,
    std::function<void(std::string)> on_notify) {
  auto p = std::shared_ptr<SoraSignaling>(
      new SoraSignaling(ioc, manager, config, std::move(on_notify)));
  if (!p->Init()) {
    return nullptr;
  }
  return p;
}

SoraSignaling::SoraSignaling(boost::asio::io_context& ioc,
                             RTCManager* manager,
                             SoraSignalingConfig config,
                             std::function<void(std::string)> on_notify)
    : ioc_(ioc),
      manager_(manager),
      config_(config),
      on_notify_(std::move(on_notify)) {}

bool SoraSignaling::Init() {
  return true;
}

void SoraSignaling::Release() {
  // connection_ を nullptr にした上で解放する
  // デストラクタ中にコールバックが呼ばれて解放中の connection_ にアクセスしてしまうことがあるため
  auto connection = std::move(connection_);
  connection = nullptr;
}

bool SoraSignaling::Connect() {
  RTC_LOG(LS_INFO) << __FUNCTION__;

  if (connected_) {
    return false;
  }

  std::string port = "443";
  if (!parts_.port.empty()) {
    port = parts_.port;
  }

  RTC_LOG(LS_INFO) << "Connect to " << parts_.host;

  URLParts parts;
  if (!URLParts::Parse(config_.signaling_url, parts)) {
    RTC_LOG(LS_ERROR) << "Invalid Signaling URL: " << config_.signaling_url;
    return false;
  }
  if (parts.scheme == "ws") {
    ws_.reset(new Websocket(ioc_));
  } else if (parts.scheme == "wss") {
    ws_.reset(new Websocket(Websocket::ssl_tag(), ioc_, config_.insecure));
  } else {
    return false;
  }

  ws_->Connect(config_.signaling_url,
               std::bind(&SoraSignaling::OnConnect, shared_from_this(),
                         std::placeholders::_1));

  return true;
}

void SoraSignaling::OnConnect(boost::system::error_code ec) {
  RTC_LOG(LS_INFO) << __FUNCTION__;

  if (ec) {
    RTC_LOG(LS_ERROR) << "Failed Websocket handshake: " << ec;
    return;
  }

  connected_ = true;
  RTC_LOG(LS_INFO) << "Signaling Websocket is connected";

  DoRead();
  DoSendConnect();
}

#define SORA_CLIENT \
  "Sora Unity SDK " SORA_UNITY_SDK_VERSION " (" SORA_UNITY_SDK_COMMIT_SHORT ")"
#define LIBWEBRTC                                                      \
  "Shiguredo-build " WEBRTC_READABLE_VERSION " (" WEBRTC_BUILD_VERSION \
  " " WEBRTC_SRC_COMMIT_SHORT ")"

void SoraSignaling::DoSendConnect() {
  std::string role = config_.role == SoraSignalingConfig::Role::Sendonly
                         ? "sendonly"
                         : config_.role == SoraSignalingConfig::Role::Recvonly
                               ? "recvonly"
                               : "sendrecv";

  boost::json::object json_message = {
      {"type", "connect"},
      {"role", role},
      {"multistream", config_.multistream},
      {"channel_id", config_.channel_id},
      {"sora_client", SORA_CLIENT},
      {"libwebrtc", LIBWEBRTC},
      {"environment",
       "Unity " + config_.unity_version + " for " SORA_UNITY_SDK_PLATFORM},
  };

  if (!config_.metadata.is_null()) {
    json_message["metadata"] = config_.metadata;
  }

  json_message["video"] = boost::json::object();
  json_message["video"].as_object()["codec_type"] = config_.video_codec;
  if (config_.video_bitrate != 0) {
    json_message["video"].as_object()["bit_rate"] = config_.video_bitrate;
  }

  json_message["audio"] = boost::json::object();
  json_message["audio"].as_object()["codec_type"] = config_.audio_codec;
  if (config_.audio_bitrate != 0) {
    json_message["audio"].as_object()["bit_rate"] = config_.audio_bitrate;
  }

  ws_->WriteText(boost::json::serialize(json_message));
}
void SoraSignaling::DoSendPong() {
  boost::json::value json_message = {{"type", "pong"}};
  ws_->WriteText(boost::json::serialize(json_message));
}
void SoraSignaling::DoSendPong(
    const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
  std::string stats = report->ToJson();
  std::string str = R"({"type":"pong","stats":)" + stats + "}";
  ws_->WriteText(std::move(str));
}

void SoraSignaling::CreatePeerFromConfig(boost::json::value jconfig) {
  webrtc::PeerConnectionInterface::RTCConfiguration rtc_config;
  webrtc::PeerConnectionInterface::IceServers ice_servers;

  auto jservers = jconfig.at("iceServers");
  for (auto jserver : jservers.as_array()) {
    const std::string username = jserver.at("username").as_string().c_str();
    const std::string credential = jserver.at("credential").as_string().c_str();
    auto jurls = jserver.at("urls");
    for (const auto url : jurls.as_array()) {
      webrtc::PeerConnectionInterface::IceServer ice_server;
      ice_server.uri = url.as_string().c_str();
      ice_server.username = username;
      ice_server.password = credential;
      ice_servers.push_back(ice_server);
    }
  }

  rtc_config.servers = ice_servers;

  connection_ = manager_->createConnection(rtc_config, this);
}

void SoraSignaling::Close() {
  ws_->Close(std::bind(&SoraSignaling::OnClose, shared_from_this(),
                       std::placeholders::_1));
}

void SoraSignaling::OnClose(boost::system::error_code ec) {
  if (ec) {
    RTC_LOG(LS_ERROR) << "Failed to close: ec=" << ec;
    return;
  }
}

void SoraSignaling::DoRead() {
  ws_->Read(std::bind(&SoraSignaling::OnRead, shared_from_this(),
                      std::placeholders::_1, std::placeholders::_2,
                      std::placeholders::_3));
}

void SoraSignaling::OnRead(boost::system::error_code ec,
                           std::size_t bytes_transferred,
                           std::string text) {
  boost::ignore_unused(bytes_transferred);

  if (ec == boost::asio::error::operation_aborted) {
    return;
  }

  if (ec) {
    RTC_LOG(LS_ERROR) << "Failed to read: ec=" << ec;
    return;
  }

  RTC_LOG(LS_INFO) << __FUNCTION__ << ": text=" << text;

  auto json_message = boost::json::parse(text);
  const std::string type = json_message.at("type").as_string().c_str();
  if (type == "offer") {
    CreatePeerFromConfig(json_message.at("config"));
    const std::string sdp = json_message.at("sdp").as_string().c_str();
    connection_->SetOffer(sdp, [this, json_message]() {
      connection_->CreateAnswer(
          [this](webrtc::SessionDescriptionInterface* desc) {
            std::string sdp;
            desc->ToString(&sdp);
            boost::json::value json_message = {{"type", "answer"},
                                               {"sdp", sdp}};
            ws_->WriteText(boost::json::serialize(json_message));
          });
    });
  } else if (type == "update") {
    const std::string sdp = json_message.at("sdp").as_string().c_str();
    connection_->SetOffer(sdp, [this]() {
      connection_->CreateAnswer(
          [this](webrtc::SessionDescriptionInterface* desc) {
            std::string sdp;
            desc->ToString(&sdp);
            boost::json::value json_message = {{"type", "update"},
                                               {"sdp", sdp}};
            ws_->WriteText(boost::json::serialize(json_message));
          });
    });
  } else if (type == "notify") {
    if (on_notify_) {
      on_notify_(text);
    }
  } else if (type == "ping") {
    if (rtc_state_ != webrtc::PeerConnectionInterface::IceConnectionState::
                          kIceConnectionConnected) {
      DoRead();
      return;
    }
    auto it = json_message.as_object().find("stats");
    if (it != json_message.as_object().end() && it->value().as_bool()) {
      connection_->GetStats(
          [this](
              const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
            DoSendPong(report);
          });
    } else {
      DoSendPong();
    }
  }

  DoRead();
}

// WebRTC からのコールバック
// これらは別スレッドからやってくるので取り扱い注意
void SoraSignaling::OnIceConnectionStateChange(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << " state:" << new_state;
  boost::asio::post(boost::beast::bind_front_handler(
      &SoraSignaling::DoIceConnectionStateChange, shared_from_this(),
      new_state));
}
void SoraSignaling::OnIceCandidate(const std::string sdp_mid,
                                   const int sdp_mlineindex,
                                   const std::string sdp) {
  boost::json::value json_message = {{"type", "candidate"}, {"candidate", sdp}};
  ws_->WriteText(boost::json::serialize(json_message));
}
void SoraSignaling::DoIceConnectionStateChange(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
  RTC_LOG(LS_INFO) << __FUNCTION__
                   << ": oldState=" << iceConnectionStateToString(rtc_state_)
                   << ", newState=" << iceConnectionStateToString(new_state);

  switch (new_state) {
    case webrtc::PeerConnectionInterface::IceConnectionState::
        kIceConnectionConnected:
      break;
    case webrtc::PeerConnectionInterface::IceConnectionState::
        kIceConnectionFailed:
      break;
    default:
      break;
  }
  rtc_state_ = new_state;
}

}  // namespace sora
