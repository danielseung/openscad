#pragma once

#include <QJsonArray>
#include <QWidget>
#include <QString>
#include <vector>

#include "gui/qtgettext.h"  // IWYU pragma: keep (needed before ui_AiChat.h for q_ macro)
#include "ui_AiChat.h"

class AnthropicClient;
class MainWindow;

class AiChat : public QWidget, public Ui::AiChatWidget
{
  Q_OBJECT

public:
  explicit AiChat(QWidget *parent = nullptr);
  void setMainWindow(MainWindow *mw);

signals:
  void requestApplyCode(const QString& code);

public slots:
  void onEditorContentChanged();

private slots:
  void onSendClicked();
  void onStopClicked();
  void onApplyClicked();
  void onClearClicked();
  void onApiResponseChunk(const QString& chunk);
  void onApiResponseComplete(const QString& fullResponse);
  void onApiError(const QString& error);

private:
  MainWindow *mainWindow = nullptr;
  AnthropicClient *apiClient;

  struct Message {
    QString role;
    QString content;
  };
  std::vector<Message> conversationHistory;
  QString currentEditorContent;
  QString lastConsoleOutput;
  QString lastExtractedCode;
  bool isStreaming = false;

  void appendMessage(const QString& role, const QString& content);
  void appendStreamingChunk(const QString& chunk);
  void finalizeStreamingMessage();
  void setInputEnabled(bool enabled);
  QString buildSystemPrompt() const;
  QJsonArray buildConversationJson() const;
  QString buildUserMessageWithContext(const QString& userText) const;
  QString extractCodeBlock(const QString& response) const;
  bool eventFilter(QObject *obj, QEvent *event) override;
};
