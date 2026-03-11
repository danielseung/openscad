/*
 *  OpenSCAD (www.openscad.org)
 *  Copyright The OpenSCAD Developers.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public
 *  License along with this program; if not, see
 *  <https://www.gnu.org/licenses/>.
 */
#pragma once

#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>

class AnthropicClient : public QObject
{
  Q_OBJECT

public:
  explicit AnthropicClient(QObject *parent = nullptr);

  void setApiKey(const QString& key);
  void setModel(const QString& model);
  void setMaxTokens(int tokens);
  void sendMessage(const QString& systemPrompt, const QJsonArray& conversationHistory);
  void abort();
  bool isBusy() const;

signals:
  void responseChunk(const QString& textDelta);
  void responseComplete(const QString& fullResponse);
  void responseUsage(int inputTokens, int outputTokens);
  void responseTruncated();
  void errorOccurred(const QString& errorMessage);

private slots:
  void onReadyRead();
  void onFinished();

private:
  QNetworkAccessManager *nam;
  QNetworkReply *currentReply = nullptr;
  QString apiKey;
  QString model;
  int maxTokens = 8192;
  QString accumulatedResponse;
  QByteArray sseBuffer;
  bool hadError = false;
  QString stopReason;

  void parseSseEvent(const QByteArray& eventData);
};
