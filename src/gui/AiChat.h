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
  void onConsoleMessage(const QString& text, bool isError);
  void onCompileFinished(int errors, int warnings);

private slots:
  void onSendClicked();
  void onStopClicked();
  void onApplyClicked();
  void onClearClicked();
  void onAutoApplyToggled(bool checked);
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
  static constexpr int MAX_HISTORY_TURNS = 20;

  QString currentEditorContent;
  QString currentFileName;
  QStringList consoleErrors;  // ring buffer of recent error/warning lines
  static constexpr int MAX_CONSOLE_LINES = 50;

  QString lastExtractedCode;
  bool isStreaming = false;
  QString streamingBuffer;  // accumulates full response during streaming
  int lastCompileErrors = 0;
  int lastCompileWarnings = 0;

  void appendMessage(const QString& role, const QString& content);
  void renderStreamingResponse();
  void finalizeStreamingMessage();
  void setInputEnabled(bool enabled);
  void updateContextLabel();
  void trimConversationHistory();
  QString buildSystemPrompt() const;
  QJsonArray buildConversationJson() const;
  QString buildUserMessageWithContext(const QString& userText) const;
  QString extractCodeBlock(const QString& response) const;
  bool eventFilter(QObject *obj, QEvent *event) override;
};
