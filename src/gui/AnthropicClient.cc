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
#include "gui/AnthropicClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QUrl>

AnthropicClient::AnthropicClient(QObject *parent)
  : QObject(parent), nam(new QNetworkAccessManager(this))
{
}

void AnthropicClient::setApiKey(const QString& key)
{
  apiKey = key;
}

void AnthropicClient::setModel(const QString& m)
{
  model = m;
}

void AnthropicClient::sendMessage(const QString& systemPrompt, const QJsonArray& conversationHistory)
{
  if (currentReply) {
    abort();
  }

  if (apiKey.isEmpty()) {
    emit errorOccurred("No API key set. Go to Edit > Preferences > AI Assistant to configure.");
    return;
  }

  accumulatedResponse.clear();
  sseBuffer.clear();
  hadError = false;

  QJsonObject body;
  body["model"] = model;
  body["max_tokens"] = 4096;
  body["stream"] = true;
  body["system"] = systemPrompt;
  body["messages"] = conversationHistory;

  QNetworkRequest request(QUrl("https://api.anthropic.com/v1/messages"));
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  request.setRawHeader("x-api-key", apiKey.toUtf8());
  request.setRawHeader("anthropic-version", "2023-06-01");

  currentReply = nam->post(request, QJsonDocument(body).toJson());
  connect(currentReply, &QNetworkReply::readyRead, this, &AnthropicClient::onReadyRead);
  connect(currentReply, &QNetworkReply::finished, this, &AnthropicClient::onFinished);
}

void AnthropicClient::abort()
{
  if (currentReply) {
    currentReply->abort();
    currentReply->deleteLater();
    currentReply = nullptr;
  }
}

bool AnthropicClient::isBusy() const
{
  return currentReply != nullptr;
}

void AnthropicClient::onReadyRead()
{
  if (!currentReply || hadError) return;

  // Check for HTTP error status
  int statusCode = currentReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (statusCode != 200 && statusCode != 0) {
    hadError = true;
    accumulatedResponse.clear();  // prevent responseComplete from firing

    // Read whatever error body is available (may be partial — onFinished handles the rest)
    QByteArray errorBody = currentReply->readAll();
    QString errorMsg;
    QJsonDocument errorDoc = QJsonDocument::fromJson(errorBody);
    if (!errorDoc.isNull() && errorDoc.isObject()) {
      QJsonObject errObj = errorDoc.object();
      if (errObj.contains("error")) {
        errorMsg = errObj["error"].toObject()["message"].toString();
      }
    }

    if (statusCode == 401) {
      emit errorOccurred("Invalid API key. Check Edit > Preferences > AI Assistant.");
    } else if (statusCode == 429) {
      emit errorOccurred("Rate limited. Try again in a moment.");
    } else if (statusCode >= 500) {
      emit errorOccurred("Claude API temporarily unavailable." +
                         (errorMsg.isEmpty() ? "" : " " + errorMsg));
    } else {
      emit errorOccurred(QString("API error %1: %2").arg(statusCode).arg(
                           errorMsg.isEmpty() ? "Unknown error" : errorMsg));
    }
    return;  // let onFinished handle cleanup — don't call abort() here
  }

  sseBuffer.append(currentReply->readAll());

  // Process complete SSE events (separated by double newlines — handle both \r\n and \n)
  while (true) {
    int idx = sseBuffer.indexOf("\r\n\r\n");
    int advance = 4;
    if (idx == -1) {
      idx = sseBuffer.indexOf("\n\n");
      advance = 2;
    }
    if (idx == -1) break;

    QByteArray event = sseBuffer.left(idx);
    sseBuffer.remove(0, idx + advance);

    if (!event.trimmed().isEmpty()) {
      parseSseEvent(event);
    }
  }
}

void AnthropicClient::onFinished()
{
  if (!currentReply) return;

  // Process any remaining data in buffer (only if no error already handled)
  if (!hadError && !sseBuffer.trimmed().isEmpty()) {
    parseSseEvent(sseBuffer);
    sseBuffer.clear();
  }

  if (!hadError) {
    if (currentReply->error() == QNetworkReply::OperationCanceledError) {
      // User aborted — don't emit error
    } else if (currentReply->error() != QNetworkReply::NoError && accumulatedResponse.isEmpty()) {
      emit errorOccurred("Network error: " + currentReply->errorString());
    }

    if (!accumulatedResponse.isEmpty()) {
      emit responseComplete(accumulatedResponse);
    }
  }

  currentReply->deleteLater();
  currentReply = nullptr;
}

void AnthropicClient::parseSseEvent(const QByteArray& eventData)
{
  // Parse SSE: each line is "field: value"
  // We care about "data:" lines
  // Handle both \r\n and \n line endings
  for (const QByteArray& rawLine : eventData.split('\n')) {
    QByteArray line = rawLine.trimmed();  // strips \r as well
    if (!line.startsWith("data: ")) continue;

    QByteArray jsonData = line.mid(6); // skip "data: "
    if (jsonData.trimmed() == "[DONE]") continue;

    QJsonDocument doc = QJsonDocument::fromJson(jsonData);
    if (doc.isNull() || !doc.isObject()) continue;

    QJsonObject obj = doc.object();
    QString type = obj["type"].toString();

    if (type == "content_block_delta") {
      QJsonObject delta = obj["delta"].toObject();
      if (delta["type"].toString() == "text_delta") {
        QString text = delta["text"].toString();
        accumulatedResponse += text;
        emit responseChunk(text);
      }
    } else if (type == "message_stop") {
      // Message complete — handled in onFinished
    } else if (type == "error") {
      QJsonObject error = obj["error"].toObject();
      hadError = true;
      accumulatedResponse.clear();
      emit errorOccurred("API error: " + error["message"].toString());
    }
  }
}
