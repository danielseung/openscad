#include "gui/AiChat.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QPalette>
#include <QRegularExpression>
#include <QScrollBar>

#include "core/Settings.h"
#include "gui/AiChatConstants.h"
#include "gui/AnthropicClient.h"
#include "gui/MainWindow.h"
#include "gui/qtgettext.h"  // IWYU pragma: keep

AiChat::AiChat(QWidget *parent) : QWidget(parent)
{
  setupUi(this);

  apiClient = new AnthropicClient(this);

  connect(sendButton, &QPushButton::clicked, this, &AiChat::onSendClicked);
  connect(stopButton, &QPushButton::clicked, this, &AiChat::onStopClicked);
  connect(applyCodeButton, &QPushButton::clicked, this, &AiChat::onApplyClicked);
  connect(clearButton, &QPushButton::clicked, this, &AiChat::onClearClicked);

  connect(apiClient, &AnthropicClient::responseChunk, this, &AiChat::onApiResponseChunk);
  connect(apiClient, &AnthropicClient::responseComplete, this, &AiChat::onApiResponseComplete);
  connect(apiClient, &AnthropicClient::errorOccurred, this, &AiChat::onApiError);

  // Install event filter on input for Ctrl+Enter
  inputText->installEventFilter(this);

  // Load auto-apply setting
  autoApplyCheckBox->setChecked(Settings::SettingsAi::aiAutoApply.value());
}

void AiChat::setMainWindow(MainWindow *mw)
{
  mainWindow = mw;
}

bool AiChat::eventFilter(QObject *obj, QEvent *event)
{
  if (obj == inputText && event->type() == QEvent::KeyPress) {
    auto *keyEvent = static_cast<QKeyEvent *>(event);
    if ((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) &&
        (keyEvent->modifiers() & Qt::ControlModifier)) {
      onSendClicked();
      return true;
    }
  }
  return QWidget::eventFilter(obj, event);
}

void AiChat::onEditorContentChanged()
{
  if (mainWindow && mainWindow->activeEditor) {
    currentEditorContent = mainWindow->activeEditor->toPlainText();
  }
}

void AiChat::onSendClicked()
{
  QString userText = inputText->toPlainText().trimmed();
  if (userText.isEmpty()) return;

  // Update API client settings from preferences
  apiClient->setApiKey(QString::fromStdString(Settings::SettingsAi::anthropicApiKey.value()));
  apiClient->setModel(QString::fromStdString(Settings::SettingsAi::anthropicModel.value()));

  // Grab current editor content
  onEditorContentChanged();

  // Build user message with context
  QString contextualMessage = buildUserMessageWithContext(userText);

  // Add to conversation history
  conversationHistory.push_back({"user", contextualMessage});

  // Display the user's raw message (without context XML)
  appendMessage("user", userText);

  // Clear input
  inputText->clear();

  // Build conversation JSON
  QJsonArray messages = buildConversationJson();

  // Send request
  setInputEnabled(false);
  apiClient->sendMessage(buildSystemPrompt(), messages);
}

void AiChat::onStopClicked()
{
  apiClient->abort();
  setInputEnabled(true);
  finalizeStreamingMessage();
}

void AiChat::onApplyClicked()
{
  if (!lastExtractedCode.isEmpty()) {
    emit requestApplyCode(lastExtractedCode);
  }
}

void AiChat::onClearClicked()
{
  conversationHistory.clear();
  chatDisplay->clear();
  lastExtractedCode.clear();
  isStreaming = false;
  applyCodeButton->setVisible(false);
  autoApplyCheckBox->setVisible(false);
}

void AiChat::onApiResponseChunk(const QString& chunk)
{
  appendStreamingChunk(chunk);
}

void AiChat::onApiResponseComplete(const QString& fullResponse)
{
  // Add assistant response to conversation history
  conversationHistory.push_back({"assistant", fullResponse});

  finalizeStreamingMessage();
  setInputEnabled(true);

  // Try to extract code
  lastExtractedCode = extractCodeBlock(fullResponse);
  if (!lastExtractedCode.isEmpty()) {
    applyCodeButton->setVisible(true);
    autoApplyCheckBox->setVisible(true);

    // Auto-apply if enabled
    if (autoApplyCheckBox->isChecked()) {
      emit requestApplyCode(lastExtractedCode);
    }
  } else {
    applyCodeButton->setVisible(false);
    autoApplyCheckBox->setVisible(false);
  }
}

void AiChat::onApiError(const QString& error)
{
  appendMessage("error", error);
  setInputEnabled(true);
  finalizeStreamingMessage();
}

void AiChat::appendMessage(const QString& role, const QString& content)
{
  // Derive colors from the current palette so they work in both light and dark themes
  const QPalette &pal = QApplication::palette();
  QColor baseBg = pal.color(QPalette::Base);
  QColor textColor = pal.color(QPalette::Text);
  bool isDark = baseBg.lightnessF() < 0.5;

  // User message: slightly tinted background
  QColor userBg = isDark ? baseBg.lighter(140) : baseBg.darker(108);
  // Assistant message: slightly different tint
  QColor assistBg = isDark ? baseBg.lighter(120) : baseBg.darker(104);
  // Error: reddish tint
  QColor errorBg = isDark ? QColor(80, 30, 30) : QColor(255, 235, 238);
  QColor errorText = isDark ? QColor(255, 130, 130) : QColor(198, 40, 40);

  QString html;
  if (role == "user") {
    html = QString(
      "<div style='margin:8px 0; padding:6px 10px; "
      "background-color:%1; color:%2; border-radius:6px;'>"
      "<b>You:</b><br>%3</div>"
    ).arg(userBg.name(), textColor.name(), content.toHtmlEscaped().replace("\n", "<br>"));
  } else if (role == "assistant") {
    QString formatted = content.toHtmlEscaped().replace("\n", "<br>");
    html = QString(
      "<div style='margin:8px 0; padding:6px 10px; "
      "background-color:%1; color:%2; border-radius:6px;'>"
      "<b>Claude:</b><br>%3</div>"
    ).arg(assistBg.name(), textColor.name(), formatted);
  } else if (role == "error") {
    html = QString(
      "<div style='margin:8px 0; padding:6px 10px; "
      "background-color:%1; border-radius:6px; color:%2;'>"
      "<b>Error:</b> %3</div>"
    ).arg(errorBg.name(), errorText.name(), content.toHtmlEscaped());
  }

  chatDisplay->append(html);
  chatDisplay->verticalScrollBar()->setValue(chatDisplay->verticalScrollBar()->maximum());
}

void AiChat::appendStreamingChunk(const QString& chunk)
{
  if (!isStreaming) {
    isStreaming = true;

    const QPalette &pal = QApplication::palette();
    QColor baseBg = pal.color(QPalette::Base);
    QColor textColor = pal.color(QPalette::Text);
    bool isDark = baseBg.lightnessF() < 0.5;
    QColor assistBg = isDark ? baseBg.lighter(120) : baseBg.darker(104);

    chatDisplay->append(QString(
      "<div style='margin:8px 0; padding:6px 10px; "
      "background-color:%1; color:%2; border-radius:6px;'>"
      "<b>Claude:</b><br>"
    ).arg(assistBg.name(), textColor.name()));
  }

  // Append the chunk as plain text at the end
  QTextCursor cursor = chatDisplay->textCursor();
  cursor.movePosition(QTextCursor::End);
  cursor.insertText(chunk);

  chatDisplay->verticalScrollBar()->setValue(chatDisplay->verticalScrollBar()->maximum());
}

void AiChat::finalizeStreamingMessage()
{
  if (!isStreaming) return;
  isStreaming = false;

  QTextCursor cursor = chatDisplay->textCursor();
  cursor.movePosition(QTextCursor::End);
  cursor.insertHtml("</div>");
  chatDisplay->verticalScrollBar()->setValue(chatDisplay->verticalScrollBar()->maximum());
}

void AiChat::setInputEnabled(bool enabled)
{
  inputText->setEnabled(enabled);
  sendButton->setVisible(enabled);
  stopButton->setVisible(!enabled);
}

QString AiChat::buildSystemPrompt() const
{
  return AiChatConstants::SYSTEM_PROMPT;
}

QJsonArray AiChat::buildConversationJson() const
{
  QJsonArray messages;
  for (const auto& msg : conversationHistory) {
    QJsonObject msgObj;
    msgObj["role"] = msg.role;
    msgObj["content"] = msg.content;
    messages.append(msgObj);
  }
  return messages;
}

QString AiChat::buildUserMessageWithContext(const QString& userText) const
{
  QString contextual;

  if (editorChip->isChecked() && !currentEditorContent.isEmpty()) {
    QString editorContent = currentEditorContent;
    // Truncate if very large (roughly 8000 tokens ~ 32000 chars)
    if (editorContent.length() > 32000) {
      editorContent = editorContent.left(32000) + "\n... (truncated)";
    }
    contextual += QString("<current_file>\n%1\n</current_file>\n\n").arg(editorContent);
  }

  if (errorsChip->isChecked() && !lastConsoleOutput.isEmpty()) {
    // Take last 50 lines of console output
    QStringList lines = lastConsoleOutput.split('\n');
    if (lines.size() > 50) {
      lines = lines.mid(lines.size() - 50);
    }
    contextual += QString("<console_output>\n%1\n</console_output>\n\n").arg(lines.join('\n'));
  }

  contextual += userText;
  return contextual;
}

QString AiChat::extractCodeBlock(const QString& response) const
{
  // Match ```scad or ```openscad or plain ``` code blocks
  QRegularExpression re(R"(```(?:openscad|scad)?\s*\n(.*?)```)",
                        QRegularExpression::DotMatchesEverythingOption);
  QRegularExpressionMatch match = re.match(response);
  if (match.hasMatch()) {
    return match.captured(1).trimmed();
  }
  return {};
}
