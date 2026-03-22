/*
 *  OpenSCAD (www.openscad.org)
 *  Copyright The OpenSCAD Developers.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */
#include "gui/ApiServer.h"

#include <QBuffer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>

#include "core/Settings.h"
#include "glview/Camera.h"
#include "glview/ColorMap.h"
#include "glview/RenderSettings.h"
#include "gui/Editor.h"
#include "gui/MainWindow.h"
#include "gui/QGLView.h"
#include "gui/TabManager.h"
#include "io/export.h"
#include "utils/printutils.h"

ApiServer::ApiServer(QObject *parent) : QObject(parent)
{
  tcpServer = new QTcpServer(this);
  connect(tcpServer, &QTcpServer::newConnection, this, &ApiServer::onNewConnection);
}

ApiServer::~ApiServer()
{
  stop();
}

void ApiServer::setMainWindow(MainWindow *mw)
{
  mainWindow = mw;
}

bool ApiServer::start(int port)
{
  if (tcpServer->isListening()) return true;

  if (!tcpServer->listen(QHostAddress::LocalHost, port)) {
    LOG("API Server: failed to listen on port %1$d: %2$s",
        port, tcpServer->errorString().toStdString());
    return false;
  }

  LOG("API Server: listening on http://127.0.0.1:%1$d", port);
  return true;
}

void ApiServer::stop()
{
  if (tcpServer->isListening()) {
    tcpServer->close();
    LOG("API Server: stopped");
  }
}

void ApiServer::onNewConnection()
{
  while (auto *socket = tcpServer->nextPendingConnection()) {
    buffers[socket] = QByteArray();
    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
      processSocket(socket);
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
      buffers.remove(socket);
      socket->deleteLater();
    });
  }
}

void ApiServer::processSocket(QTcpSocket *socket)
{
  buffers[socket].append(socket->readAll());
  const QByteArray& data = buffers[socket];

  // Wait for end of headers
  int headerEnd = data.indexOf("\r\n\r\n");
  if (headerEnd < 0) return;

  // Parse headers to get Content-Length
  HttpRequest req;
  if (!parseHttpRequest(data, req)) {
    sendError(socket, 400, "Bad Request");
    return;
  }

  int contentLength = req.headers.value("content-length", "0").toInt();
  int bodyStart = headerEnd + 4;
  if (data.size() - bodyStart < contentLength) return; // wait for more data

  req.body = data.mid(bodyStart, contentLength);
  buffers.remove(socket);

  if (!checkAuth(req)) {
    sendError(socket, 401, "Unauthorized");
    return;
  }

  handleRequest(socket, req);
}

bool ApiServer::parseHttpRequest(const QByteArray& raw, HttpRequest& req)
{
  int headerEnd = raw.indexOf("\r\n\r\n");
  if (headerEnd < 0) return false;

  QByteArray headerSection = raw.left(headerEnd);
  QList<QByteArray> lines = headerSection.split('\n');
  if (lines.isEmpty()) return false;

  // Parse request line: "GET /api/editor HTTP/1.1\r"
  QByteArray requestLine = lines[0].trimmed();
  QList<QByteArray> parts = requestLine.split(' ');
  if (parts.size() < 2) return false;

  req.method = QString::fromUtf8(parts[0]);
  req.path = QString::fromUtf8(parts[1]);

  // Strip query string
  int queryIdx = req.path.indexOf('?');
  if (queryIdx >= 0) req.path = req.path.left(queryIdx);

  // Parse headers
  for (int i = 1; i < lines.size(); ++i) {
    QByteArray line = lines[i].trimmed();
    int colonIdx = line.indexOf(':');
    if (colonIdx > 0) {
      QString key = QString::fromUtf8(line.left(colonIdx)).trimmed().toLower();
      QString value = QString::fromUtf8(line.mid(colonIdx + 1)).trimmed();
      req.headers[key] = value;
    }
  }

  return true;
}

void ApiServer::sendResponse(QTcpSocket *socket, int statusCode,
                             const QByteArray& body, const QString& contentType)
{
  QString statusText;
  switch (statusCode) {
    case 200: statusText = "OK"; break;
    case 400: statusText = "Bad Request"; break;
    case 401: statusText = "Unauthorized"; break;
    case 404: statusText = "Not Found"; break;
    case 405: statusText = "Method Not Allowed"; break;
    case 500: statusText = "Internal Server Error"; break;
    default: statusText = "Unknown"; break;
  }

  QByteArray response;
  response.append(QString("HTTP/1.1 %1 %2\r\n").arg(statusCode).arg(statusText).toUtf8());
  response.append(QString("Content-Type: %1\r\n").arg(contentType).toUtf8());
  response.append(QString("Content-Length: %1\r\n").arg(body.size()).toUtf8());
  response.append("Access-Control-Allow-Origin: *\r\n");
  response.append("Connection: close\r\n");
  response.append("\r\n");
  response.append(body);

  socket->write(response);
  socket->flush();
  socket->disconnectFromHost();
}

void ApiServer::sendError(QTcpSocket *socket, int code, const QString& msg)
{
  QJsonObject obj;
  obj["error"] = msg;
  obj["code"] = code;
  sendResponse(socket, code, QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

bool ApiServer::checkAuth(const HttpRequest& req)
{
  QString token = QString::fromStdString(
    Settings::SettingsApi::apiAuthToken.value());
  if (token.isEmpty()) return true; // no auth required

  QString authHeader = req.headers.value("authorization", "");
  return authHeader == QString("Bearer %1").arg(token);
}

void ApiServer::handleRequest(QTcpSocket *socket, const HttpRequest& req)
{
  // CORS preflight
  if (req.method == "OPTIONS") {
    QByteArray body = "{}";
    QByteArray response;
    response.append("HTTP/1.1 204 No Content\r\n");
    response.append("Access-Control-Allow-Origin: *\r\n");
    response.append("Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n");
    response.append("Access-Control-Allow-Headers: Content-Type, Authorization\r\n");
    response.append("Content-Length: 0\r\n");
    response.append("Connection: close\r\n");
    response.append("\r\n");
    socket->write(response);
    socket->flush();
    socket->disconnectFromHost();
    return;
  }

  if (!mainWindow) {
    sendError(socket, 500, "MainWindow not available");
    return;
  }

  // Route requests
  if (req.method == "GET") {
    if (req.path == "/api/editor") {
      sendResponse(socket, 200, handleGetEditor());
    } else if (req.path == "/api/console") {
      sendResponse(socket, 200, handleGetConsole());
    } else if (req.path == "/api/viewport/screenshot") {
      QByteArray png = handleGetViewportScreenshot();
      sendResponse(socket, 200, png, "image/png");
    } else if (req.path == "/api/viewport/camera") {
      sendResponse(socket, 200, handleGetViewportCamera());
    } else if (req.path == "/api/compile/status") {
      sendResponse(socket, 200, handleGetCompileStatus());
    } else if (req.path == "/api/status") {
      sendResponse(socket, 200, handleGetStatus());
    } else {
      sendError(socket, 404, "Not Found");
    }
  } else if (req.method == "POST") {
    if (req.path == "/api/editor") {
      sendResponse(socket, 200, handlePostEditor(req.body));
    } else if (req.path == "/api/editor/insert") {
      sendResponse(socket, 200, handlePostEditorInsert(req.body));
    } else if (req.path == "/api/compile/preview") {
      sendResponse(socket, 200, handlePostCompilePreview());
    } else if (req.path == "/api/compile/render") {
      sendResponse(socket, 200, handlePostCompileRender());
    } else if (req.path == "/api/viewport/camera") {
      sendResponse(socket, 200, handlePostViewportCamera(req.body));
    } else if (req.path == "/api/viewport/viewall") {
      sendResponse(socket, 200, handlePostViewportViewAll());
    } else if (req.path == "/api/file/save") {
      sendResponse(socket, 200, handlePostFileSave());
    } else if (req.path == "/api/file/saveas") {
      sendResponse(socket, 200, handlePostFileSaveAs(req.body));
    } else if (req.path == "/api/file/open") {
      sendResponse(socket, 200, handlePostFileOpen(req.body));
    } else if (req.path == "/api/export") {
      sendResponse(socket, 200, handlePostExport(req.body));
    } else {
      sendError(socket, 404, "Not Found");
    }
  } else {
    sendError(socket, 405, "Method Not Allowed");
  }
}

// --- GET Handlers ---

QByteArray ApiServer::handleGetEditor()
{
  QJsonObject obj;
  if (mainWindow->activeEditor) {
    obj["code"] = mainWindow->activeEditor->toPlainText();
    obj["filepath"] = mainWindow->activeEditor->filepath;
  } else {
    obj["code"] = "";
    obj["filepath"] = "";
    obj["error"] = "No active editor";
  }
  return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QByteArray ApiServer::handleGetConsole()
{
  QJsonObject obj;
  if (mainWindow->console) {
    obj["text"] = mainWindow->console->toPlainText();
  } else {
    obj["text"] = "";
  }
  return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QByteArray ApiServer::handleGetViewportScreenshot()
{
  if (!mainWindow->qglview) return QByteArray();

  const QImage& frame = mainWindow->qglview->grabFrame();
  QByteArray pngData;
  QBuffer buffer(&pngData);
  buffer.open(QIODevice::WriteOnly);
  frame.save(&buffer, "PNG");
  return pngData;
}

QByteArray ApiServer::handleGetViewportCamera()
{
  QJsonObject obj;
  if (mainWindow->qglview) {
    auto& cam = mainWindow->qglview->cam;
    auto vpt = cam.getVpt();
    auto vpr = cam.getVpr();

    QJsonArray vptArr;
    vptArr.append(vpt[0]);
    vptArr.append(vpt[1]);
    vptArr.append(vpt[2]);

    QJsonArray vprArr;
    vprArr.append(vpr[0]);
    vprArr.append(vpr[1]);
    vprArr.append(vpr[2]);

    obj["vpt"] = vptArr;
    obj["vpr"] = vprArr;
    obj["vpd"] = cam.zoomValue();
    obj["vpf"] = cam.fovValue();
  }
  return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QByteArray ApiServer::handleGetCompileStatus()
{
  QJsonObject obj;
  obj["errors"] = mainWindow->compileErrors;
  obj["warnings"] = mainWindow->compileWarnings;
  return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QByteArray ApiServer::handleGetStatus()
{
  QJsonObject obj;

  // Editor info
  QJsonObject editor;
  if (mainWindow->activeEditor) {
    editor["filepath"] = mainWindow->activeEditor->filepath;
    editor["modified"] = mainWindow->activeEditor->isContentModified();
    editor["length"] = mainWindow->activeEditor->toPlainText().length();
  }
  obj["editor"] = editor;

  // Compile info
  QJsonObject compile;
  compile["errors"] = mainWindow->compileErrors;
  compile["warnings"] = mainWindow->compileWarnings;
  obj["compile"] = compile;

  // Camera info
  if (mainWindow->qglview) {
    auto& cam = mainWindow->qglview->cam;
    auto vpt = cam.getVpt();
    auto vpr = cam.getVpr();

    QJsonObject camera;
    QJsonArray vptArr, vprArr;
    vptArr.append(vpt[0]); vptArr.append(vpt[1]); vptArr.append(vpt[2]);
    vprArr.append(vpr[0]); vprArr.append(vpr[1]); vprArr.append(vpr[2]);
    camera["vpt"] = vptArr;
    camera["vpr"] = vprArr;
    camera["vpd"] = cam.zoomValue();
    camera["vpf"] = cam.fovValue();
    obj["camera"] = camera;
  }

  obj["hasRenderedGeometry"] = (mainWindow->rootGeom != nullptr);

  return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

// --- POST Handlers ---

QByteArray ApiServer::handlePostEditor(const QByteArray& body)
{
  QJsonObject obj;
  QJsonDocument doc = QJsonDocument::fromJson(body);
  if (!doc.isObject() || !doc.object().contains("code")) {
    obj["error"] = "Missing 'code' field";
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  }

  QString code = doc.object()["code"].toString();
  if (mainWindow->activeEditor) {
    mainWindow->activeEditor->setText(code);
    obj["ok"] = true;
  } else {
    obj["error"] = "No active editor";
  }
  return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QByteArray ApiServer::handlePostEditorInsert(const QByteArray& body)
{
  QJsonObject obj;
  QJsonDocument doc = QJsonDocument::fromJson(body);
  if (!doc.isObject() || !doc.object().contains("text")) {
    obj["error"] = "Missing 'text' field";
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  }

  QString text = doc.object()["text"].toString();
  if (mainWindow->activeEditor) {
    mainWindow->activeEditor->insert(text);
    obj["ok"] = true;
  } else {
    obj["error"] = "No active editor";
  }
  return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QByteArray ApiServer::handlePostCompilePreview()
{
  QJsonObject obj;
  if (GuiLocker::isLocked()) {
    obj["error"] = "Compile already in progress";
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  }
  mainWindow->actionRenderPreview();
  obj["status"] = "started";
  return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QByteArray ApiServer::handlePostCompileRender()
{
  QJsonObject obj;
  if (GuiLocker::isLocked()) {
    obj["error"] = "Compile already in progress";
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  }
  // Trigger F6 render via the menu action (public slot via auto-connect)
  QMetaObject::invokeMethod(mainWindow, "on_designActionRender_triggered");
  obj["status"] = "started";
  return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QByteArray ApiServer::handlePostViewportCamera(const QByteArray& body)
{
  QJsonObject obj;
  if (!mainWindow->qglview) {
    obj["error"] = "Viewport not available";
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  }
  QJsonDocument doc = QJsonDocument::fromJson(body);
  if (!doc.isObject()) {
    obj["error"] = "Invalid JSON";
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  }

  auto json = doc.object();
  auto& cam = mainWindow->qglview->cam;

  if (json.contains("vpt")) {
    auto arr = json["vpt"].toArray();
    if (arr.size() == 3) {
      cam.setVpt(arr[0].toDouble(), arr[1].toDouble(), arr[2].toDouble());
    }
  }
  if (json.contains("vpr")) {
    auto arr = json["vpr"].toArray();
    if (arr.size() == 3) {
      cam.setVpr(arr[0].toDouble(), arr[1].toDouble(), arr[2].toDouble());
    }
  }
  if (json.contains("vpd")) {
    cam.setVpd(json["vpd"].toDouble());
  }
  if (json.contains("vpf")) {
    cam.setVpf(json["vpf"].toDouble());
  }

  mainWindow->qglview->update();
  obj["ok"] = true;
  return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QByteArray ApiServer::handlePostViewportViewAll()
{
  QJsonObject obj;
  mainWindow->qglview->viewAll();
  mainWindow->qglview->update();
  obj["ok"] = true;
  return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QByteArray ApiServer::handlePostFileSave()
{
  QJsonObject obj;
  if (!mainWindow->activeEditor) {
    obj["error"] = "No active editor";
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  }
  if (mainWindow->activeEditor->filepath.isEmpty()) {
    obj["error"] = "File has no path. Use /api/file/saveas with a path instead.";
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  }
  bool result = mainWindow->tabManager->save(mainWindow->activeEditor);
  obj["ok"] = result;
  if (!result) obj["error"] = "Save failed";
  return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QByteArray ApiServer::handlePostFileSaveAs(const QByteArray& body)
{
  QJsonObject obj;
  QJsonDocument doc = QJsonDocument::fromJson(body);
  if (!doc.isObject() || !doc.object().contains("path")) {
    obj["error"] = "Missing 'path' field";
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  }

  QString path = doc.object()["path"].toString();
  if (!mainWindow->activeEditor) {
    obj["error"] = "No active editor";
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  }

  // Validate parent directory exists and is writable
  QFileInfo pathInfo(path);
  QFileInfo dirInfo(pathInfo.absolutePath());
  if (!dirInfo.exists()) {
    obj["error"] = QString("Directory does not exist: %1").arg(pathInfo.absolutePath());
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  }
  if (!dirInfo.isWritable()) {
    obj["error"] = QString("Directory is not writable: %1").arg(pathInfo.absolutePath());
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  }

  bool result = mainWindow->tabManager->saveAs(mainWindow->activeEditor, path);
  obj["ok"] = result;
  if (!result) obj["error"] = "Save failed";
  return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QByteArray ApiServer::handlePostFileOpen(const QByteArray& body)
{
  QJsonObject obj;
  QJsonDocument doc = QJsonDocument::fromJson(body);
  if (!doc.isObject() || !doc.object().contains("path")) {
    obj["error"] = "Missing 'path' field";
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  }

  QString path = doc.object()["path"].toString();
  if (!QFileInfo::exists(path)) {
    obj["error"] = QString("File not found: %1").arg(path);
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  }
  mainWindow->tabManager->open(path);
  obj["ok"] = true;
  return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QByteArray ApiServer::handlePostExport(const QByteArray& body)
{
  QJsonObject obj;
  QJsonDocument doc = QJsonDocument::fromJson(body);
  if (!doc.isObject()) {
    obj["error"] = "Invalid JSON";
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  }

  auto json = doc.object();
  if (!json.contains("format") || !json.contains("path")) {
    obj["error"] = "Missing 'format' and/or 'path' fields";
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  }

  if (!mainWindow->rootGeom) {
    obj["error"] = "No rendered geometry. Run render (F6) first.";
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  }

  // Warn if editor content has changed since last render
  if (mainWindow->activeEditor && !mainWindow->activeEditor->contentsRendered) {
    obj["warning"] = "Editor content modified since last render. Export may not match current code. Run render (F6) again.";
  }

  QString formatStr = json["format"].toString().toLower();
  QString path = json["path"].toString();

  // Resolve format from identifier string (e.g., "stl", "off", "amf", "3mf")
  FileFormat format;
  if (!fileformat::fromIdentifier(formatStr.toStdString(), format)) {
    // Try common aliases
    if (formatStr == "stl") format = FileFormat::BINARY_STL;
    else if (formatStr == "off") format = FileFormat::OFF;
    else if (formatStr == "amf") format = FileFormat::AMF;
    else if (formatStr == "3mf") format = FileFormat::_3MF;
    else if (formatStr == "obj") format = FileFormat::OBJ;
    else if (formatStr == "dxf") format = FileFormat::DXF;
    else if (formatStr == "svg") format = FileFormat::SVG;
    else if (formatStr == "pdf") format = FileFormat::PDF;
    else {
      obj["error"] = QString("Unknown format: %1. Supported: stl, off, amf, 3mf, obj, dxf, svg, pdf").arg(formatStr);
      return QJsonDocument(obj).toJson(QJsonDocument::Compact);
    }
  }

  // Validate output path
  QFileInfo exportPathInfo(path);
  QFileInfo exportDirInfo(exportPathInfo.absolutePath());
  if (!exportDirInfo.exists() || !exportDirInfo.isWritable()) {
    obj["error"] = QString("Cannot write to: %1").arg(exportPathInfo.absolutePath());
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  }

  // Check 2D/3D compatibility
  if (fileformat::is2D(format) && mainWindow->rootGeom->getDimension() == 3) {
    obj["error"] = QString("Cannot export 3D geometry to 2D format '%1'. Use stl, off, amf, 3mf, or obj.").arg(formatStr);
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  }
  if (fileformat::is3D(format) && mainWindow->rootGeom->getDimension() == 2) {
    obj["error"] = QString("Cannot export 2D geometry to 3D format '%1'. Use dxf or svg.").arg(formatStr);
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
  }

  auto& fmtInfo = fileformat::info(format);
  auto colorScheme = ColorMap::inst()->findColorScheme(RenderSettings::inst()->colorscheme);
  ExportInfo exportInfo{
    .format = format,
    .info = fmtInfo,
    .title = path.toStdString(),
    .sourceFilePath = mainWindow->activeEditor ? mainWindow->activeEditor->filepath.toStdString() : "",
    .camera = &mainWindow->qglview->cam,
    .defaultColor = ColorMap::getColor(*colorScheme, RenderColor::CGAL_FACE_FRONT_COLOR),
    .colorScheme = colorScheme,
  };

  bool result = exportFileByName(mainWindow->rootGeom, path.toStdString(), exportInfo);
  obj["ok"] = result;
  if (!result) obj["error"] = "Export failed";
  return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}
