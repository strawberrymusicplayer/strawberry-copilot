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

#ifndef DISCORDRPC_H
#define DISCORDRPC_H

#include "config.h"

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QLocalSocket>
#include <QTimer>

#include <cstdint>

namespace discord {

struct DiscordPresence {
  int type;
  int status_display_type;
  QString name;
  QString state;
  QString details;
  qint64 start_timestamp;
  qint64 end_timestamp;
  QString large_image_key;
  QString large_image_text;
  QString small_image_key;
  QString small_image_text;
  QString party_id;
  int party_size;
  int party_max;
  int party_privacy;
  QString match_secret;
  QString join_secret;
  QString spectate_secret;
  bool instance;

  DiscordPresence()
      : type(0),
        status_display_type(0),
        start_timestamp(0),
        end_timestamp(0),
        party_size(0),
        party_max(0),
        party_privacy(0),
        instance(false) {}
};

class DiscordRPC : public QObject {
  Q_OBJECT

 public:
  explicit DiscordRPC(const QString &application_id, QObject *parent = nullptr);
  ~DiscordRPC() override;

  void Initialize();
  void Shutdown();
  void UpdatePresence(const DiscordPresence &presence);
  void ClearPresence();

  bool IsConnected() const { return state_ == State::Connected; }

 private Q_SLOTS:
  void OnConnected();
  void OnDisconnected();
  void OnReadyRead();
  void OnError(QLocalSocket::LocalSocketError error);
  void OnReconnectTimer();

 private:
  enum class State {
    Disconnected,
    Connecting,
    SentHandshake,
    Connected
  };

  enum class Opcode : quint32 {
    Handshake = 0,
    Frame = 1,
    Close = 2,
    Ping = 3,
    Pong = 4
  };

  void ConnectToDiscord();
  void SendHandshake();
  void SendFrame(const QByteArray &data);
  void ProcessIncomingData();
  void HandleMessage(const QByteArray &data);
  void ScheduleReconnect();
  QByteArray CreateHandshakeMessage();
  QByteArray CreatePresenceMessage(const DiscordPresence &presence);

  QString application_id_;
  QLocalSocket *socket_;
  QTimer *reconnect_timer_;
  State state_;
  int nonce_;
  int reconnect_delay_;
  QByteArray read_buffer_;
};

}  // namespace discord

#endif  // DISCORDRPC_H
