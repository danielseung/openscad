/*
 *  OpenSCAD (www.openscad.org)
 *  Copyright The OpenSCAD Developers.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */
#pragma once

#include <QByteArray>
#include <QMap>
#include <QObject>
#include <QString>

class QTcpServer;
class QTcpSocket;
class MainWindow;

class ApiServer : public QObject
{
  Q_OBJECT

public:
  explicit ApiServer(QObject *parent = nullptr);
  ~ApiServer() override;

  void setMainWindow(MainWindow *mw);
  bool start(int port);
  void stop();

private slots:
  void onNewConnection();

private:
  struct HttpRequest {
    QString method;
    QString path;
    QMap<QString, QString> headers;
    QByteArray body;
  };

  void processSocket(QTcpSocket *socket);
  bool parseHttpRequest(const QByteArray& raw, HttpRequest& req);
  void sendResponse(QTcpSocket *socket, int statusCode,
                    const QByteArray& body,
                    const QString& contentType = "application/json");
  void sendError(QTcpSocket *socket, int code, const QString& msg);
  bool checkAuth(const HttpRequest& req);
  void handleRequest(QTcpSocket *socket, const HttpRequest& req);

  // GET handlers
  QByteArray handleGetEditor();
  QByteArray handleGetConsole();
  QByteArray handleGetViewportScreenshot();
  QByteArray handleGetViewportCamera();
  QByteArray handleGetCompileStatus();
  QByteArray handleGetStatus();

  // POST handlers
  QByteArray handlePostEditor(const QByteArray& body);
  QByteArray handlePostEditorInsert(const QByteArray& body);
  QByteArray handlePostCompilePreview();
  QByteArray handlePostCompileRender();
  QByteArray handlePostViewportCamera(const QByteArray& body);
  QByteArray handlePostViewportViewAll();
  QByteArray handlePostFileSave();
  QByteArray handlePostFileSaveAs(const QByteArray& body);
  QByteArray handlePostFileOpen(const QByteArray& body);
  QByteArray handlePostExport(const QByteArray& body);

  QTcpServer *tcpServer = nullptr;
  MainWindow *mainWindow = nullptr;
  QMap<QTcpSocket*, QByteArray> buffers;
};
