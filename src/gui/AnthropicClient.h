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
  void sendMessage(const QString& systemPrompt, const QJsonArray& conversationHistory);
  void abort();
  bool isBusy() const;

signals:
  void responseChunk(const QString& textDelta);
  void responseComplete(const QString& fullResponse);
  void errorOccurred(const QString& errorMessage);

private slots:
  void onReadyRead();
  void onFinished();

private:
  QNetworkAccessManager *nam;
  QNetworkReply *currentReply = nullptr;
  QString apiKey;
  QString model;
  QString accumulatedResponse;
  QByteArray sseBuffer;

  void parseSseEvent(const QByteArray& eventData);
};
