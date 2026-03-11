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
#include "gui/AiChat.h"

#include <QApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QPalette>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextBlock>

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
  connect(autoApplyCheckBox, &QCheckBox::toggled, this, &AiChat::onAutoApplyToggled);

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

// --- Public slots for MainWindow integration ---

void AiChat::onEditorContentChanged()
{
  if (mainWindow && mainWindow->activeEditor) {
    currentEditorContent = mainWindow->activeEditor->toPlainText();
    // Get filename from the editor's filepath
    const QString& editorPath = mainWindow->activeEditor->filepath;
    if (!editorPath.isEmpty()) {
      currentFileName = QFileInfo(editorPath).fileName();
    } else {
      currentFileName = "Untitled.scad";
    }
    updateContextLabel();
  }
}

void AiChat::onConsoleMessage(const QString& text, bool isError)
{
  if (!isError) return;  // only collect errors and warnings

  consoleErrors.append(text);
  while (consoleErrors.size() > MAX_CONSOLE_LINES) {
    consoleErrors.removeFirst();
  }
}

void AiChat::onCompileFinished(int errors, int warnings)
{
  lastCompileErrors = errors;
  lastCompileWarnings = warnings;

  // Auto-check the Errors chip when compile fails
  if (errors > 0) {
    errorsChip->setChecked(true);
  }
}

// --- Private slots ---

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

  // Add to conversation history and trim if too long
  conversationHistory.push_back({"user", contextualMessage});
  trimConversationHistory();

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
  consoleErrors.clear();
  streamingBuffer.clear();
  isStreaming = false;
  applyCodeButton->setVisible(false);
  autoApplyCheckBox->setVisible(false);
}

void AiChat::onAutoApplyToggled(bool checked)
{
  Settings::SettingsAi::aiAutoApply.setValue(checked);
}

void AiChat::onApiResponseChunk(const QString& chunk)
{
  streamingBuffer += chunk;
  renderStreamingResponse();
}

void AiChat::onApiResponseComplete(const QString& fullResponse)
{
  // Add assistant response to conversation history
  conversationHistory.push_back({"assistant", fullResponse});

  finalizeStreamingMessage();
  setInputEnabled(true);

  // Try to extract code — take the LAST code block (most likely the complete file)
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
  // Finalize any in-progress streaming BEFORE showing the error
  finalizeStreamingMessage();
  appendMessage("error", error);
  setInputEnabled(true);
}

// --- Display helpers ---

void AiChat::appendMessage(const QString& role, const QString& content)
{
  const QPalette &pal = QApplication::palette();
  QColor baseBg = pal.color(QPalette::Base);
  QColor textColor = pal.color(QPalette::Text);
  bool isDark = baseBg.lightnessF() < 0.5;

  QColor userBg = isDark ? baseBg.lighter(140) : baseBg.darker(108);
  QColor assistBg = isDark ? baseBg.lighter(120) : baseBg.darker(104);
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

void AiChat::renderStreamingResponse()
{
  // Instead of mixing HTML append + plain text insert (which breaks Qt's rich text model),
  // we re-render the full accumulated streaming buffer each time.
  // This is slightly less efficient but produces correct, consistent HTML.

  const QPalette &pal = QApplication::palette();
  QColor baseBg = pal.color(QPalette::Base);
  QColor textColor = pal.color(QPalette::Text);
  bool isDark = baseBg.lightnessF() < 0.5;
  QColor assistBg = isDark ? baseBg.lighter(120) : baseBg.darker(104);

  QString formatted = streamingBuffer.toHtmlEscaped().replace("\n", "<br>");
  QString html = QString(
    "<div style='margin:8px 0; padding:6px 10px; "
    "background-color:%1; color:%2; border-radius:6px;'>"
    "<b>Claude:</b><br>%3</div>"
  ).arg(assistBg.name(), textColor.name(), formatted);

  if (!isStreaming) {
    // First chunk — just append the block
    isStreaming = true;
    chatDisplay->append(html);
  } else {
    // Subsequent chunks — replace the last block
    // Remove the last paragraph block and replace with updated content
    QTextCursor cursor = chatDisplay->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
    // Select back to find the start of our streaming block
    // We look for the start by going back to the block that contains "Claude:"
    QTextDocument *doc = chatDisplay->document();
    QTextBlock block = doc->lastBlock();
    while (block.isValid()) {
      if (block.text().contains("Claude:")) {
        cursor.setPosition(block.position());
        cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        break;
      }
      block = block.previous();
    }
    cursor.removeSelectedText();
    cursor.insertHtml(html);
  }

  chatDisplay->verticalScrollBar()->setValue(chatDisplay->verticalScrollBar()->maximum());
}

void AiChat::finalizeStreamingMessage()
{
  if (!isStreaming) return;
  isStreaming = false;
  streamingBuffer.clear();
  // The streaming block is already a complete, well-formed HTML div — nothing to close
}

void AiChat::setInputEnabled(bool enabled)
{
  inputText->setEnabled(enabled);
  sendButton->setVisible(enabled);
  stopButton->setVisible(!enabled);
}

void AiChat::updateContextLabel()
{
  if (currentFileName.isEmpty()) {
    contextLabel->setText("Context:");
  } else {
    int chars = currentEditorContent.length();
    contextLabel->setText(QString("Context: %1 (%2 chars)").arg(currentFileName).arg(chars));
  }
}

void AiChat::trimConversationHistory()
{
  // Keep at most MAX_HISTORY_TURNS pairs (user+assistant) to avoid blowing context
  while (conversationHistory.size() > static_cast<size_t>(MAX_HISTORY_TURNS) * 2) {
    // Remove the oldest pair (first user message + first assistant response)
    conversationHistory.erase(conversationHistory.begin());
    if (!conversationHistory.empty()) {
      conversationHistory.erase(conversationHistory.begin());
    }
  }
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
    QString nameAttr = currentFileName.isEmpty() ? "" : QString(" name=\"%1\"").arg(currentFileName);
    contextual += QString("<current_file%1>\n%2\n</current_file>\n\n").arg(nameAttr, editorContent);
  }

  if (errorsChip->isChecked() && !consoleErrors.isEmpty()) {
    contextual += QString("<errors count=\"%1\">\n%2\n</errors>\n\n")
                    .arg(consoleErrors.size())
                    .arg(consoleErrors.join('\n'));
  }

  if (lastCompileErrors > 0 || lastCompileWarnings > 0) {
    contextual += QString("<compile_state errors=\"%1\" warnings=\"%2\"/>\n\n")
                    .arg(lastCompileErrors).arg(lastCompileWarnings);
  }

  contextual += userText;
  return contextual;
}

QString AiChat::extractCodeBlock(const QString& response) const
{
  // Match ```scad or ```openscad or plain ``` code blocks
  // Use globalMatch to find ALL blocks, then take the LAST one
  // (most likely the complete, corrected file)
  QRegularExpression re(R"(```(?:openscad|scad)?\s*\n(.*?)```)",
                        QRegularExpression::DotMatchesEverythingOption);
  QRegularExpressionMatchIterator it = re.globalMatch(response);
  QString lastMatch;
  while (it.hasNext()) {
    QRegularExpressionMatch match = it.next();
    lastMatch = match.captured(1).trimmed();
  }
  return lastMatch;
}
