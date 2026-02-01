/*
 * Strawberry Music Player
 * Copyright 2025, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "discordrpc.h"

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QLocalSocket>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QDir>

#ifdef Q_OS_WIN
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace discord {

namespace {
constexpr int kReconnectMinDelayMs = 500;
constexpr int kReconnectMaxDelayMs = 60000;
constexpr int kRpcVersion = 1;
constexpr size_t kMaxFrameSize = 64 * 1024;
}  // namespace

DiscordRPC::DiscordRPC(const QString &application_id, QObject *parent)
    : QObject(parent),
      application_id_(application_id),
      socket_(new QLocalSocket(this)),
      reconnect_timer_(new QTimer(this)),
      state_(State::Disconnected),
      nonce_(1),
      reconnect_delay_(kReconnectMinDelayMs) {

  QObject::connect(socket_, &QLocalSocket::connected, this, &DiscordRPC::OnConnected);
  QObject::connect(socket_, &QLocalSocket::disconnected, this, &DiscordRPC::OnDisconnected);
  QObject::connect(socket_, &QLocalSocket::readyRead, this, &DiscordRPC::OnReadyRead);
  QObject::connect(socket_, &QLocalSocket::errorOccurred, this, &DiscordRPC::OnError);

  reconnect_timer_->setSingleShot(true);
  QObject::connect(reconnect_timer_, &QTimer::timeout, this, &DiscordRPC::OnReconnectTimer);

}

DiscordRPC::~DiscordRPC() {
  Shutdown();
}

void DiscordRPC::Initialize() {

  if (state_ != State::Disconnected) {
    return;
  }

  ConnectToDiscord();

}

void DiscordRPC::Shutdown() {

  reconnect_timer_->stop();
  if (socket_->state() != QLocalSocket::UnconnectedState) {
    socket_->disconnectFromServer();
  }
  state_ = State::Disconnected;

}

void DiscordRPC::ConnectToDiscord() {

  if (state_ != State::Disconnected) {
    return;
  }

  state_ = State::Connecting;

#ifdef Q_OS_WIN
  // On Windows, Discord uses named pipes
  for (int i = 0; i < 10; ++i) {
    const QString pipe_name = QString("\\\\.\\pipe\\discord-ipc-%1").arg(i);
    socket_->connectToServer(pipe_name);
    if (socket_->waitForConnected(100)) {
      return;  // Connected successfully
    }
  }
#else
  // On Unix-like systems, Discord uses Unix domain sockets in temp directory
  QStringList temp_paths;
  
  // Check various temp directories
  const QString xdg_runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
  if (!xdg_runtime.isEmpty()) temp_paths << xdg_runtime;
  
  const QString tmpdir = qEnvironmentVariable("TMPDIR");
  if (!tmpdir.isEmpty()) temp_paths << tmpdir;
  
  const QString tmp = qEnvironmentVariable("TMP");
  if (!tmp.isEmpty()) temp_paths << tmp;
  
  const QString temp = qEnvironmentVariable("TEMP");
  if (!temp.isEmpty()) temp_paths << temp;
  
  temp_paths << "/tmp";

  for (const QString &temp_path : temp_paths) {
    for (int i = 0; i < 10; ++i) {
      const QString socket_path = QString("%1/discord-ipc-%2").arg(temp_path).arg(i);
      socket_->connectToServer(socket_path);
      if (socket_->waitForConnected(100)) {
        return;  // Connected successfully
      }
    }
  }
#endif

  // Connection failed, schedule reconnect
  state_ = State::Disconnected;
  ScheduleReconnect();

}

void DiscordRPC::OnConnected() {

  state_ = State::SentHandshake;
  reconnect_delay_ = kReconnectMinDelayMs;
  SendHandshake();

}

void DiscordRPC::OnDisconnected() {

  state_ = State::Disconnected;
  read_buffer_.clear();
  ScheduleReconnect();

}

void DiscordRPC::OnError(QLocalSocket::LocalSocketError error) {

  Q_UNUSED(error);
  // Error handling is done in OnDisconnected

}

void DiscordRPC::OnReconnectTimer() {

  ConnectToDiscord();

}

void DiscordRPC::ScheduleReconnect() {

  reconnect_timer_->start(reconnect_delay_);
  reconnect_delay_ = qMin(reconnect_delay_ * 2, kReconnectMaxDelayMs);

}

void DiscordRPC::SendHandshake() {

  QByteArray handshake_data = CreateHandshakeMessage();

  // Create frame header
  QByteArray frame;
  quint32 opcode = static_cast<quint32>(Opcode::Handshake);
  quint32 length = static_cast<quint32>(handshake_data.size());

  frame.append(reinterpret_cast<const char*>(&opcode), sizeof(opcode));
  frame.append(reinterpret_cast<const char*>(&length), sizeof(length));
  frame.append(handshake_data);

  socket_->write(frame);
  socket_->flush();

}

void DiscordRPC::SendFrame(const QByteArray &data) {

  if (state_ != State::Connected) {
    return;
  }

  QByteArray frame;
  quint32 opcode = static_cast<quint32>(Opcode::Frame);
  quint32 length = static_cast<quint32>(data.size());

  frame.append(reinterpret_cast<const char*>(&opcode), sizeof(opcode));
  frame.append(reinterpret_cast<const char*>(&length), sizeof(length));
  frame.append(data);

  socket_->write(frame);
  socket_->flush();

}

void DiscordRPC::OnReadyRead() {

  read_buffer_.append(socket_->readAll());
  ProcessIncomingData();

}

void DiscordRPC::ProcessIncomingData() {

  while (read_buffer_.size() >= static_cast<int>(sizeof(quint32) * 2)) {
    // Read frame header
    quint32 opcode;
    quint32 length;

    memcpy(&opcode, read_buffer_.constData(), sizeof(opcode));
    memcpy(&length, read_buffer_.constData() + sizeof(opcode), sizeof(length));

    // Check if we have the full message
    const int header_size = sizeof(opcode) + sizeof(length);
    if (read_buffer_.size() < header_size + static_cast<int>(length)) {
      return;  // Wait for more data
    }

    // Extract message data
    QByteArray message_data = read_buffer_.mid(header_size, length);
    read_buffer_.remove(0, header_size + length);

    // Process message based on opcode
    Opcode op = static_cast<Opcode>(opcode);
    switch (op) {
      case Opcode::Frame:
        HandleMessage(message_data);
        break;
      case Opcode::Close:
        socket_->disconnectFromServer();
        break;
      case Opcode::Ping: {
        // Respond with Pong
        QByteArray pong_frame;
        quint32 pong_opcode = static_cast<quint32>(Opcode::Pong);
        pong_frame.append(reinterpret_cast<const char*>(&pong_opcode), sizeof(pong_opcode));
        pong_frame.append(reinterpret_cast<const char*>(&length), sizeof(length));
        pong_frame.append(message_data);
        socket_->write(pong_frame);
        socket_->flush();
        break;
      }
      case Opcode::Pong:
      case Opcode::Handshake:
      default:
        // Ignore
        break;
    }
  }

}

void DiscordRPC::HandleMessage(const QByteArray &data) {

  QJsonParseError error;
  QJsonDocument doc = QJsonDocument::fromJson(data, &error);

  if (error.error != QJsonParseError::NoError || !doc.isObject()) {
    return;
  }

  QJsonObject obj = doc.object();

  if (state_ == State::SentHandshake) {
    // Check for READY event
    QString cmd = obj.value("cmd").toString();
    QString evt = obj.value("evt").toString();

    if (cmd == "DISPATCH" && evt == "READY") {
      state_ = State::Connected;
    }
  }

}

QByteArray DiscordRPC::CreateHandshakeMessage() {

  QJsonObject obj;
  obj["v"] = kRpcVersion;
  obj["client_id"] = application_id_;

  QJsonDocument doc(obj);
  return doc.toJson(QJsonDocument::Compact);

}

QByteArray DiscordRPC::CreatePresenceMessage(const DiscordPresence &presence) {

  QJsonObject obj;
  obj["cmd"] = "SET_ACTIVITY";
  obj["nonce"] = QString::number(nonce_++);

  QJsonObject args;
#ifdef Q_OS_WIN
  args["pid"] = static_cast<qint64>(GetCurrentProcessId());
#else
  args["pid"] = static_cast<qint64>(getpid());
#endif

  QJsonObject activity;

  if (presence.type >= 0 && presence.type <= 5) {
    activity["type"] = presence.type;
    activity["status_display_type"] = presence.status_display_type;
  }

  if (!presence.name.isEmpty()) {
    activity["name"] = presence.name;
  }
  if (!presence.state.isEmpty()) {
    activity["state"] = presence.state;
  }
  if (!presence.details.isEmpty()) {
    activity["details"] = presence.details;
  }

  if (presence.start_timestamp > 0 || presence.end_timestamp > 0) {
    QJsonObject timestamps;
    if (presence.start_timestamp > 0) {
      timestamps["start"] = presence.start_timestamp;
    }
    if (presence.end_timestamp > 0) {
      timestamps["end"] = presence.end_timestamp;
    }
    activity["timestamps"] = timestamps;
  }

  if (!presence.large_image_key.isEmpty() || !presence.large_image_text.isEmpty() ||
      !presence.small_image_key.isEmpty() || !presence.small_image_text.isEmpty()) {
    QJsonObject assets;
    if (!presence.large_image_key.isEmpty()) {
      assets["large_image"] = presence.large_image_key;
    }
    if (!presence.large_image_text.isEmpty()) {
      assets["large_text"] = presence.large_image_text;
    }
    if (!presence.small_image_key.isEmpty()) {
      assets["small_image"] = presence.small_image_key;
    }
    if (!presence.small_image_text.isEmpty()) {
      assets["small_text"] = presence.small_image_text;
    }
    activity["assets"] = assets;
  }

  if (!presence.party_id.isEmpty() || presence.party_size > 0 || presence.party_max > 0 || presence.party_privacy > 0) {
    QJsonObject party;
    if (!presence.party_id.isEmpty()) {
      party["id"] = presence.party_id;
    }
    if (presence.party_size > 0 && presence.party_max > 0) {
      QJsonArray size;
      size.append(presence.party_size);
      size.append(presence.party_max);
      party["size"] = size;
    }
    if (presence.party_privacy > 0) {
      party["privacy"] = presence.party_privacy;
    }
    activity["party"] = party;
  }

  if (!presence.match_secret.isEmpty() || !presence.join_secret.isEmpty() || !presence.spectate_secret.isEmpty()) {
    QJsonObject secrets;
    if (!presence.match_secret.isEmpty()) {
      secrets["match"] = presence.match_secret;
    }
    if (!presence.join_secret.isEmpty()) {
      secrets["join"] = presence.join_secret;
    }
    if (!presence.spectate_secret.isEmpty()) {
      secrets["spectate"] = presence.spectate_secret;
    }
    activity["secrets"] = secrets;
  }

  activity["instance"] = presence.instance;

  args["activity"] = activity;
  obj["args"] = args;

  QJsonDocument doc(obj);
  return doc.toJson(QJsonDocument::Compact);

}

void DiscordRPC::UpdatePresence(const DiscordPresence &presence) {

  if (state_ == State::Connected) {
    QByteArray data = CreatePresenceMessage(presence);
    SendFrame(data);
  }

}

void DiscordRPC::ClearPresence() {

  DiscordPresence empty_presence;
  UpdatePresence(empty_presence);

}

}  // namespace discord
