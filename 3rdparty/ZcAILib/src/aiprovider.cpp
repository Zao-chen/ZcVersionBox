#include "aiprovider.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {

QString buildApiErrorMessage(const QByteArray &data, const QString &fallback) {
  QString errorMsg = fallback;

  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
  if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
    return errorMsg;
  }

  const QJsonObject root = doc.object();
  if (!root.contains("error") || !root.value("error").isObject()) {
    return errorMsg;
  }

  const QString apiError =
      root.value("error").toObject().value("message").toString();
  if (!apiError.isEmpty()) {
    errorMsg += "\nAPI error: " + apiError;
  }

  return errorMsg;
}

QString extractFinalContent(const QJsonObject &root) {
  const QJsonArray choices = root.value("choices").toArray();
  if (choices.isEmpty()) {
    return {};
  }

  const QJsonObject firstChoice = choices.first().toObject();
  const QJsonObject message = firstChoice.value("message").toObject();
  if (!message.isEmpty()) {
    return message.value("content").toString();
  }

  const QJsonObject delta = firstChoice.value("delta").toObject();
  return delta.value("content").toString();
}

QString extractStreamDelta(const QJsonObject &root) {
  const QJsonArray choices = root.value("choices").toArray();
  if (choices.isEmpty()) {
    return {};
  }

  const QJsonObject delta =
      choices.first().toObject().value("delta").toObject();
  return delta.value("content").toString();
}

QString joinBaseUrl(const QString &baseUrl, const QString &path) {
  QString trimmed = baseUrl.trimmed();
  if (trimmed.endsWith('/')) {
    trimmed.chop(1);
  }

  QString suffix = path.trimmed();
  if (!suffix.startsWith('/')) {
    suffix.prepend('/');
  }

  return trimmed + suffix;
}

} // namespace

AiProvider::AiProvider(QObject *parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)),
      m_apiUrl("https://api.openai.com/v1/chat/completions"),
      m_baseUrl("https://api.openai.com/v1"), m_model("gpt-3.5-turbo"),
      m_streamEnabled(false),
      m_modelsApiUrl("https://api.openai.com/v1/models"),
      m_serviceType(OpenAI) {}

AiProvider::~AiProvider() {}

void AiProvider::setServiceType(ServiceType type) {
  m_serviceType = type;

  switch (type) {
  case OpenAI:
    m_baseUrl = "https://api.openai.com/v1";
    m_apiUrl = joinBaseUrl(m_baseUrl, "chat/completions");
    m_modelsApiUrl = joinBaseUrl(m_baseUrl, "models");
    m_model = "gpt-3.5-turbo";
    break;

  case DeepSeek:
    m_baseUrl = "https://api.deepseek.com/v1";
    m_apiUrl = joinBaseUrl(m_baseUrl, "chat/completions");
    m_modelsApiUrl = joinBaseUrl(m_baseUrl, "models");
    m_model = "deepseek-chat";
    break;

  case Custom:
    break;
  }
}

void AiProvider::setApiKey(const QString &apiKey) { m_apiKey = apiKey; }

void AiProvider::setApiUrl(const QString &url) {
  m_apiUrl = url;
  m_baseUrl.clear();
}

void AiProvider::setBaseUrl(const QString &baseUrl) {
  m_baseUrl = baseUrl.trimmed();
  if (m_baseUrl.isEmpty()) {
    return;
  }

  m_apiUrl = joinBaseUrl(m_baseUrl, "chat/completions");
  m_modelsApiUrl = joinBaseUrl(m_baseUrl, "models");
}

void AiProvider::setModel(const QString &model) { m_model = model; }

void AiProvider::setStreamEnabled(bool enabled) { m_streamEnabled = enabled; }

void AiProvider::fetchModels() {
  if (m_apiKey.isEmpty()) {
    emit errorOccurred("API Key is not set");
    return;
  }

  if (m_modelsApiUrl.isEmpty()) {
    emit errorOccurred("Models API URL is not configured");
    return;
  }

  QNetworkRequest request{QUrl(m_modelsApiUrl)};
  request.setRawHeader("Authorization", ("Bearer " + m_apiKey).toUtf8());

  qDebug() << "=== Fetching Models ===";
  qDebug() << "URL:" << m_modelsApiUrl;

  QNetworkReply *reply = m_network->get(request);
  connect(reply, &QNetworkReply::finished, this,
          &AiProvider::handleModelsReply);
}

void AiProvider::handleModelsReply() {
  QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
  if (!reply) {
    return;
  }

  const QByteArray data = reply->readAll();

  qDebug() << "=== Models Response ===";
  qDebug()
      << "Status Code:"
      << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  qDebug() << "Data:" << data;

  if (reply->error() != QNetworkReply::NoError) {
    const QString errorMsg = buildApiErrorMessage(
        data,
        QString("Fetch models failed [%1]: %2")
            .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                     .toInt())
            .arg(reply->errorString()));

    emit errorOccurred(errorMsg);
    reply->deleteLater();
    return;
  }

  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
  if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
    emit errorOccurred("Invalid models response JSON");
    reply->deleteLater();
    return;
  }

  const QJsonObject root = doc.object();
  if (!root.contains("data") || !root.value("data").isArray()) {
    emit errorOccurred("Models response is missing 'data'");
    reply->deleteLater();
    return;
  }

  QList<ModelInfo> models;
  const QJsonArray items = root.value("data").toArray();
  for (const QJsonValue &value : items) {
    const QJsonObject modelObject = value.toObject();

    ModelInfo info;
    info.id = modelObject.value("id").toString();
    info.ownedBy = modelObject.value("owned_by").toString();

    if (modelObject.contains("created")) {
      const qint64 timestamp = modelObject.value("created").toInteger();
      info.created =
          QDateTime::fromSecsSinceEpoch(timestamp).toString("yyyy-MM-dd");
    }

    if (modelObject.contains("permission") &&
        modelObject.value("permission").isArray()) {
      const QJsonArray permissions = modelObject.value("permission").toArray();
      for (const QJsonValue &permission : permissions) {
        info.permissions.append(permission.toString());
      }
    }

    models.append(info);
  }

  if (models.isEmpty()) {
    emit errorOccurred("No models were returned");
  } else {
    emit modelsReceived(models);
  }

  reply->deleteLater();
}

void AiProvider::chat(const QString &message) {
  if (m_apiKey.isEmpty()) {
    emit errorOccurred("API Key is not set");
    return;
  }

  if (m_apiUrl.isEmpty()) {
    emit errorOccurred("API URL is not set");
    return;
  }

  QNetworkRequest request{QUrl(m_apiUrl)};
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  request.setRawHeader("Authorization", ("Bearer " + m_apiKey).toUtf8());

  QJsonObject json;
  json["model"] = m_model;
  json["stream"] = m_streamEnabled;

  QJsonArray messages;
  if (!m_systemPrompt.isEmpty()) {
    QJsonObject systemMsg;
    systemMsg["role"] = "system";
    systemMsg["content"] = m_systemPrompt;
    messages.append(systemMsg);
  }

  QJsonObject userMsg;
  userMsg["role"] = "user";
  userMsg["content"] = message;
  messages.append(userMsg);
  json["messages"] = messages;

  qDebug() << "=== AI Request ===";
  qDebug() << "URL:" << m_apiUrl;
  qDebug() << "Model:" << m_model;
  qDebug() << "Stream:" << m_streamEnabled;
  qDebug() << "Message:" << message;

  QNetworkReply *reply = m_network->post(
      request, QJsonDocument(json).toJson(QJsonDocument::Compact));
  connect(reply, &QNetworkReply::finished, this, &AiProvider::handleReply);

  if (m_streamEnabled) {
    m_streamBuffers.insert(reply, QByteArray());
    m_rawResponses.insert(reply, QByteArray());
    m_streamReplies.insert(reply, QString());
    connect(reply, &QIODevice::readyRead, this,
            &AiProvider::handleStreamReadyRead);
  }
}

void AiProvider::handleStreamReadyRead() {
  QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
  if (!reply) {
    return;
  }

  processStreamChunk(reply, reply->readAll());
}

void AiProvider::processStreamChunk(QNetworkReply *reply,
                                    const QByteArray &chunk) {
  if (!reply || !m_streamBuffers.contains(reply)) {
    return;
  }

  if (!chunk.isEmpty()) {
    m_streamBuffers[reply].append(chunk);
    m_rawResponses[reply].append(chunk);
  }

  QByteArray &buffer = m_streamBuffers[reply];
  while (true) {
    const int newlineIndex = buffer.indexOf('\n');
    if (newlineIndex < 0) {
      break;
    }

    QByteArray line = buffer.left(newlineIndex);
    buffer.remove(0, newlineIndex + 1);

    if (!line.isEmpty() && line.endsWith('\r')) {
      line.chop(1);
    }

    if (line.isEmpty() || !line.startsWith("data:")) {
      continue;
    }

    const QByteArray payload = line.mid(5).trimmed();
    if (payload.isEmpty() || payload == "[DONE]") {
      continue;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
      continue;
    }

    const QJsonObject root = doc.object();
    if (root.contains("error")) {
      emit errorOccurred(buildApiErrorMessage(
          payload, "Streaming response contained an error"));
      continue;
    }

    const QString delta = extractStreamDelta(root);
    if (delta.isEmpty()) {
      continue;
    }

    m_streamReplies[reply].append(delta);
    emit replyChunkReceived(delta);
  }
}

void AiProvider::finalizeStreamReply(QNetworkReply *reply) {
  if (!reply) {
    return;
  }

  if (!m_streamBuffers.value(reply).isEmpty()) {
    processStreamChunk(reply, "\n");
  }

  QString fullReply = m_streamReplies.value(reply);
  if (fullReply.isEmpty()) {
    const QByteArray rawResponse = m_rawResponses.value(reply);
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(rawResponse, &parseError);
    if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
      fullReply = extractFinalContent(doc.object());
    }
  }

  if (fullReply.isEmpty()) {
    emit errorOccurred("Response did not contain any content");
  } else {
    emit replyReceived(fullReply);
  }

  cleanupStreamReply(reply);
}

void AiProvider::cleanupStreamReply(QNetworkReply *reply) {
  m_streamBuffers.remove(reply);
  m_rawResponses.remove(reply);
  m_streamReplies.remove(reply);
}

void AiProvider::handleReply() {
  QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
  if (!reply) {
    return;
  }

  if (m_streamBuffers.contains(reply)) {
    const QByteArray tail = reply->readAll();
    if (!tail.isEmpty()) {
      processStreamChunk(reply, tail);
    }

    if (reply->error() != QNetworkReply::NoError) {
      const QByteArray rawResponse = m_rawResponses.value(reply);
      const QString errorMsg = buildApiErrorMessage(
          rawResponse,
          QString("Network error [%1]: %2")
              .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                       .toInt())
              .arg(reply->errorString()));

      emit errorOccurred(errorMsg);
      cleanupStreamReply(reply);
      reply->deleteLater();
      return;
    }

    finalizeStreamReply(reply);
    reply->deleteLater();
    return;
  }

  const QByteArray data = reply->readAll();

  qDebug() << "=== AI Response ===";
  qDebug()
      << "Status Code:"
      << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

  if (reply->error() != QNetworkReply::NoError) {
    const QString errorMsg = buildApiErrorMessage(
        data,
        QString("Network error [%1]: %2")
            .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                     .toInt())
            .arg(reply->errorString()));

    emit errorOccurred(errorMsg);
    reply->deleteLater();
    return;
  }

  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
  if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
    emit errorOccurred("Invalid response JSON");
    reply->deleteLater();
    return;
  }

  const QJsonObject root = doc.object();
  if (root.contains("error")) {
    emit errorOccurred(buildApiErrorMessage(data, "API error"));
    reply->deleteLater();
    return;
  }

  const QString content = extractFinalContent(root);
  if (content.isEmpty()) {
    emit errorOccurred("Response did not contain any content");
  } else {
    emit replyReceived(content);
  }

  reply->deleteLater();
}

void AiProvider::setSystemPrompt(const QString &prompt) {
  m_systemPrompt = prompt;
}
