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

  QJsonObject body;
  body["model"] = model.isEmpty() ? "claude-sonnet-4-20250514" : model;
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
  if (!currentReply) return;

  // Check for HTTP error status on first data
  int statusCode = currentReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (statusCode != 200 && statusCode != 0) {
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
      emit errorOccurred("Claude API temporarily unavailable. " + errorMsg);
    } else {
      emit errorOccurred(QString("API error %1: %2").arg(statusCode).arg(errorMsg));
    }
    currentReply->abort();
    return;
  }

  sseBuffer.append(currentReply->readAll());

  // Process complete SSE events (separated by double newlines)
  while (true) {
    int idx = sseBuffer.indexOf("\n\n");
    if (idx == -1) break;

    QByteArray event = sseBuffer.left(idx);
    sseBuffer.remove(0, idx + 2);

    if (!event.trimmed().isEmpty()) {
      parseSseEvent(event);
    }
  }
}

void AnthropicClient::onFinished()
{
  if (!currentReply) return;

  // Process any remaining data in buffer
  if (!sseBuffer.trimmed().isEmpty()) {
    parseSseEvent(sseBuffer);
    sseBuffer.clear();
  }

  if (currentReply->error() == QNetworkReply::OperationCanceledError) {
    // User aborted — don't emit error
  } else if (currentReply->error() != QNetworkReply::NoError && accumulatedResponse.isEmpty()) {
    emit errorOccurred("Network error: " + currentReply->errorString());
  }

  if (!accumulatedResponse.isEmpty()) {
    emit responseComplete(accumulatedResponse);
  }

  currentReply->deleteLater();
  currentReply = nullptr;
}

void AnthropicClient::parseSseEvent(const QByteArray& eventData)
{
  // Parse SSE: each line is "field: value"
  // We care about "data:" lines
  for (const QByteArray& line : eventData.split('\n')) {
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
      emit errorOccurred("API error: " + error["message"].toString());
    }
  }
}
