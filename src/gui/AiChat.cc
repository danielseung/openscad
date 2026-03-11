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
#include <QTextCursor>

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
  connect(apiClient, &AnthropicClient::responseUsage, this, &AiChat::onApiResponseUsage);
  connect(apiClient, &AnthropicClient::responseTruncated, this, &AiChat::onApiResponseTruncated);
  connect(apiClient, &AnthropicClient::errorOccurred, this, &AiChat::onApiError);

  // Install event filter on input for Ctrl+Enter
  inputText->installEventFilter(this);

  // Load auto-apply setting
  autoApplyCheckBox->setChecked(Settings::SettingsAi::aiAutoApply.value());

  // Set default stylesheet for chat display
  const QPalette &pal = QApplication::palette();
  QColor baseBg = pal.color(QPalette::Base);
  chatDisplay->setStyleSheet(
    QString("QTextBrowser { background-color: %1; border: none; }")
      .arg(baseBg.name()));

  // Token label starts hidden
  tokenLabel->setVisible(false);
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
    const QString& editorPath = mainWindow->activeEditor->filepath;
    QString newFileName;
    if (!editorPath.isEmpty()) {
      newFileName = QFileInfo(editorPath).fileName();
    } else {
      newFileName = "Untitled.scad";
    }

    // Detect file switch and insert a separator
    if (!previousFileName.isEmpty() && newFileName != previousFileName
        && !conversationHistory.empty()) {
      appendMessage("system",
        QString("Switched to %1").arg(newFileName));
    }
    previousFileName = currentFileName;
    currentFileName = newFileName;
    updateContextLabel();
  }
}

void AiChat::onConsoleMessage(const QString& text, bool isError)
{
  if (!isError) return;

  consoleErrors.append(text);
  while (consoleErrors.size() > MAX_CONSOLE_LINES) {
    consoleErrors.removeFirst();
  }
}

void AiChat::onCompileFinished(int errors, int warnings)
{
  lastCompileErrors = errors;
  lastCompileWarnings = warnings;

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
  apiClient->setMaxTokens(Settings::SettingsAi::maxTokens.value());

  // Grab current editor content
  onEditorContentChanged();

  // Build user message with context
  QString contextualMessage = buildUserMessageWithContext(userText);

  // Add to conversation history and trim if too long
  conversationHistory.push_back({"user", contextualMessage});
  trimConversationHistory();

  // Display the user's raw message (without context XML)
  appendMessage("user", userText);

  // Clear input and reset token display
  inputText->clear();
  lastInputTokens = 0;
  lastOutputTokens = 0;
  tokenLabel->setVisible(false);

  // Build conversation JSON
  QJsonArray messages = buildConversationJson();

  // Send request
  setInputEnabled(false);
  apiClient->sendMessage(buildSystemPrompt(), messages);
}

void AiChat::onStopClicked()
{
  apiClient->abort();

  // Remove orphaned user message that got no assistant reply
  if (!conversationHistory.empty() && conversationHistory.back().role == "user") {
    conversationHistory.pop_back();
  }

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
  streamingBlockStart = -1;
  isStreaming = false;
  previousFileName.clear();
  lastInputTokens = 0;
  lastOutputTokens = 0;
  applyCodeButton->setVisible(false);
  autoApplyCheckBox->setVisible(false);
  tokenLabel->setVisible(false);
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
  conversationHistory.push_back({"assistant", fullResponse});

  finalizeStreamingMessage();
  setInputEnabled(true);

  // Re-render the final message with markdown formatting
  if (streamingBlockStart == -1) {
    // If streaming never started (shouldn't happen), just append
    appendMessage("assistant", fullResponse);
  }

  // Show token usage
  updateTokenLabel();

  // Extract code
  lastExtractedCode = extractCodeBlock(fullResponse);
  if (!lastExtractedCode.isEmpty()) {
    applyCodeButton->setVisible(true);
    autoApplyCheckBox->setVisible(true);

    if (autoApplyCheckBox->isChecked()) {
      emit requestApplyCode(lastExtractedCode);
    }
  } else {
    applyCodeButton->setVisible(false);
    autoApplyCheckBox->setVisible(false);
  }
}

void AiChat::onApiResponseUsage(int inputTokens, int outputTokens)
{
  if (inputTokens > 0) lastInputTokens = inputTokens;
  if (outputTokens > 0) lastOutputTokens = outputTokens;
}

void AiChat::onApiResponseTruncated()
{
  appendMessage("warning",
    "Response was truncated (max tokens reached). The code may be incomplete.");
}

void AiChat::onApiError(const QString& error)
{
  finalizeStreamingMessage();
  appendMessage("error", error);
  setInputEnabled(true);
}

// --- Display helpers ---

QString AiChat::formatMarkdown(const QString& text) const
{
  QString result;
  const QPalette &pal = QApplication::palette();
  QColor baseBg = pal.color(QPalette::Base);
  bool isDark = baseBg.lightnessF() < 0.5;
  QColor codeBg = isDark ? baseBg.lighter(160) : baseBg.darker(112);
  QColor inlineCodeBg = isDark ? baseBg.lighter(150) : baseBg.darker(108);

  // Split into lines and process code blocks
  QStringList lines = text.split('\n');
  bool inCodeBlock = false;
  QString codeLanguage;
  QString codeContent;

  for (const QString& line : lines) {
    if (!inCodeBlock && line.trimmed().startsWith("```")) {
      inCodeBlock = true;
      codeLanguage = line.trimmed().mid(3).trimmed();
      codeContent.clear();
      continue;
    }

    if (inCodeBlock) {
      if (line.trimmed() == "```") {
        // End of code block — render it
        QString langLabel;
        if (!codeLanguage.isEmpty()) {
          langLabel = QString(
            "<div style='font-size:10px; color:gray; margin-bottom:2px;'>%1</div>"
          ).arg(codeLanguage.toHtmlEscaped());
        }
        result += QString(
          "<div style='background-color:%1; border-radius:4px; padding:8px; "
          "margin:6px 0; font-family:monospace; font-size:12px; "
          "white-space:pre-wrap; overflow-wrap:break-word;'>%2%3</div>"
        ).arg(codeBg.name(), langLabel, codeContent.toHtmlEscaped());
        inCodeBlock = false;
        codeLanguage.clear();
      } else {
        if (!codeContent.isEmpty()) codeContent += '\n';
        codeContent += line;
      }
      continue;
    }

    // Process inline formatting
    QString processed = line.toHtmlEscaped();

    // Bold: **text**
    processed.replace(QRegularExpression(R"(\*\*(.+?)\*\*)"), "<b>\\1</b>");

    // Inline code: `text`
    processed.replace(QRegularExpression(R"(`([^`]+)`)"),
      QString("<code style='background-color:%1; padding:1px 4px; border-radius:3px; "
              "font-family:monospace; font-size:12px;'>\\1</code>").arg(inlineCodeBg.name()));

    // Bullet points
    if (processed.trimmed().startsWith("- ") || processed.trimmed().startsWith("* ")) {
      int indent = processed.indexOf(QRegularExpression("[\\-\\*]"));
      QString content = processed.trimmed().mid(2);
      result += QString("<div style='margin-left:%1px; text-indent:-12px; padding-left:12px;'>"
                        "&#8226; %2</div>").arg(indent * 4 + 8).arg(content);
      continue;
    }

    // Headers: ## text
    if (processed.trimmed().startsWith("## ")) {
      result += QString("<div style='font-weight:bold; margin:8px 0 4px 0;'>%1</div>")
                  .arg(processed.trimmed().mid(3));
      continue;
    }

    result += processed + "<br>";
  }

  // Handle unclosed code block
  if (inCodeBlock && !codeContent.isEmpty()) {
    result += QString(
      "<div style='background-color:%1; border-radius:4px; padding:8px; "
      "margin:6px 0; font-family:monospace; font-size:12px; "
      "white-space:pre-wrap; overflow-wrap:break-word;'>%2</div>"
    ).arg(codeBg.name(), codeContent.toHtmlEscaped());
  }

  return result;
}

void AiChat::appendHtml(const QString& html)
{
  chatDisplay->append(html);
  chatDisplay->verticalScrollBar()->setValue(chatDisplay->verticalScrollBar()->maximum());
}

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
  QColor warningBg = isDark ? QColor(80, 60, 20) : QColor(255, 248, 225);
  QColor warningText = isDark ? QColor(255, 200, 100) : QColor(180, 120, 0);
  QColor systemBg = isDark ? baseBg.lighter(110) : baseBg.darker(102);
  QColor systemText = isDark ? QColor(160, 160, 160) : QColor(120, 120, 120);

  QString html;
  if (role == "user") {
    html = QString(
      "<div style='margin:6px 0; padding:8px 12px; "
      "background-color:%1; color:%2; border-radius:8px;'>"
      "<b style='color:%2;'>You</b><br>%3</div>"
    ).arg(userBg.name(), textColor.name(), content.toHtmlEscaped().replace("\n", "<br>"));
  } else if (role == "assistant") {
    html = QString(
      "<div style='margin:6px 0; padding:8px 12px; "
      "background-color:%1; color:%2; border-radius:8px;'>"
      "<b style='color:%2;'>Claude</b><br>%3</div>"
    ).arg(assistBg.name(), textColor.name(), formatMarkdown(content));
  } else if (role == "error") {
    html = QString(
      "<div style='margin:6px 0; padding:8px 12px; "
      "background-color:%1; border-radius:8px; color:%2;'>"
      "<b>Error:</b> %3</div>"
    ).arg(errorBg.name(), errorText.name(), content.toHtmlEscaped());
  } else if (role == "warning") {
    html = QString(
      "<div style='margin:6px 0; padding:8px 12px; "
      "background-color:%1; border-radius:8px; color:%2;'>"
      "<b>Warning:</b> %3</div>"
    ).arg(warningBg.name(), warningText.name(), content.toHtmlEscaped());
  } else if (role == "system") {
    html = QString(
      "<div style='margin:4px 0; padding:4px 12px; "
      "background-color:%1; border-radius:4px; color:%2; "
      "font-size:11px; font-style:italic;'>"
      "%3</div>"
    ).arg(systemBg.name(), systemText.name(), content.toHtmlEscaped());
  }

  appendHtml(html);
}

void AiChat::renderStreamingResponse()
{
  const QPalette &pal = QApplication::palette();
  QColor baseBg = pal.color(QPalette::Base);
  QColor textColor = pal.color(QPalette::Text);
  bool isDark = baseBg.lightnessF() < 0.5;
  QColor assistBg = isDark ? baseBg.lighter(120) : baseBg.darker(104);

  // During streaming, show plain escaped text (no markdown parsing for perf)
  QString formatted = streamingBuffer.toHtmlEscaped().replace("\n", "<br>");
  QString html = QString(
    "<div style='margin:6px 0; padding:8px 12px; "
    "background-color:%1; color:%2; border-radius:8px;'>"
    "<b style='color:%2;'>Claude</b><br>%3</div>"
  ).arg(assistBg.name(), textColor.name(), formatted);

  if (!isStreaming) {
    // First chunk — record position and append
    isStreaming = true;
    streamingBlockStart = chatDisplay->document()->characterCount();
    chatDisplay->append(html);
  } else {
    // Subsequent chunks — use tracked position to replace the streaming block
    QTextCursor cursor(chatDisplay->document());
    if (streamingBlockStart >= 0) {
      // Position is 1 before the block we inserted (after the trailing newline of previous content)
      int pos = qMin(streamingBlockStart, chatDisplay->document()->characterCount());
      cursor.setPosition(pos);
      cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
      cursor.removeSelectedText();
      cursor.insertHtml(html);
    }
  }

  chatDisplay->verticalScrollBar()->setValue(chatDisplay->verticalScrollBar()->maximum());
}

void AiChat::finalizeStreamingMessage()
{
  if (!isStreaming) return;

  // Re-render with full markdown formatting now that streaming is done
  if (!streamingBuffer.isEmpty()) {
    QTextCursor cursor(chatDisplay->document());
    if (streamingBlockStart >= 0) {
      int pos = qMin(streamingBlockStart, chatDisplay->document()->characterCount());
      cursor.setPosition(pos);
      cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
      cursor.removeSelectedText();

      const QPalette &pal = QApplication::palette();
      QColor baseBg = pal.color(QPalette::Base);
      QColor textColor = pal.color(QPalette::Text);
      bool isDark = baseBg.lightnessF() < 0.5;
      QColor assistBg = isDark ? baseBg.lighter(120) : baseBg.darker(104);

      QString html = QString(
        "<div style='margin:6px 0; padding:8px 12px; "
        "background-color:%1; color:%2; border-radius:8px;'>"
        "<b style='color:%2;'>Claude</b><br>%3</div>"
      ).arg(assistBg.name(), textColor.name(), formatMarkdown(streamingBuffer));

      cursor.insertHtml(html);
    }
    chatDisplay->verticalScrollBar()->setValue(chatDisplay->verticalScrollBar()->maximum());
  }

  isStreaming = false;
  streamingBuffer.clear();
  streamingBlockStart = -1;
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
    QString selectedInfo;
    QString sel = getSelectedEditorText();
    if (!sel.isEmpty()) {
      selectedInfo = QString(" | %1 selected").arg(sel.length());
    }
    contextLabel->setText(
      QString("Context: %1 (%2 chars%3)").arg(currentFileName).arg(chars).arg(selectedInfo));
  }
}

void AiChat::updateTokenLabel()
{
  if (lastInputTokens > 0 || lastOutputTokens > 0) {
    tokenLabel->setText(
      QString("Tokens: %1 in / %2 out").arg(lastInputTokens).arg(lastOutputTokens));
    tokenLabel->setVisible(true);
  }
}

void AiChat::trimConversationHistory()
{
  while (conversationHistory.size() > static_cast<size_t>(MAX_HISTORY_TURNS) * 2) {
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
  for (size_t i = 0; i < conversationHistory.size(); ++i) {
    const auto& msg = conversationHistory[i];
    QJsonObject msgObj;
    msgObj["role"] = msg.role;

    // Only include file content in the most recent user message to save tokens
    if (msg.role == "user" && i < conversationHistory.size() - 1) {
      // Strip <current_file> from older messages
      QString stripped = msg.content;
      QRegularExpression fileRe(R"(<current_file[^>]*>[\s\S]*?</current_file>\s*)",
                                QRegularExpression::DotMatchesEverythingOption);
      stripped.replace(fileRe, "[file content omitted from older turn]\n\n");
      msgObj["content"] = stripped;
    } else {
      msgObj["content"] = msg.content;
    }

    messages.append(msgObj);
  }
  return messages;
}

QString AiChat::getSelectedEditorText() const
{
  if (mainWindow && mainWindow->activeEditor) {
    return mainWindow->activeEditor->selectedText();
  }
  return QString();
}

QString AiChat::buildUserMessageWithContext(const QString& userText) const
{
  QString contextual;

  // Include selection if available, otherwise full file
  QString selection = getSelectedEditorText();

  if (editorChip->isChecked() && !currentEditorContent.isEmpty()) {
    // Escape filename for XML attribute
    QString escapedName = currentFileName;
    escapedName.replace("\"", "&quot;");
    escapedName.replace("&", "&amp;");
    QString nameAttr = escapedName.isEmpty() ? "" : QString(" name=\"%1\"").arg(escapedName);

    if (!selection.isEmpty()) {
      // Send both full file and selection, highlighting what's selected
      QString editorContent = currentEditorContent;
      if (editorContent.length() > 32000) {
        editorContent = editorContent.left(32000) + "\n... (truncated)";
      }
      contextual += QString("<current_file%1>\n%2\n</current_file>\n\n").arg(nameAttr, editorContent);
      contextual += QString("<selected_code>\n%1\n</selected_code>\n\n").arg(selection);
    } else {
      QString editorContent = currentEditorContent;
      if (editorContent.length() > 32000) {
        editorContent = editorContent.left(32000) + "\n... (truncated)";
      }
      contextual += QString("<current_file%1>\n%2\n</current_file>\n\n").arg(nameAttr, editorContent);
    }
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
