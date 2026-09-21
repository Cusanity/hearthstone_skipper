#include <memory>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>

#include "yaml-cpp/yaml.h"

#include "skipper.h"

using namespace std::string_literals;

Skipper::Skipper(ConfigAwareQEasy *qeasy, QObject *parent)
    : QObject(parent), _qeasy(qeasy), _logger(spdlog::get("skipper")) {
    assert(qeasy != nullptr);
    connect(_qeasy, &ConfigAwareQEasy::testFinished, this,
            [this](bool testSuccess, const std::string &message) { emit testFinished(testSuccess, message); });
}

Skipper::~Skipper() = default;

void Skipper::skip() {
    if (_busy) {
        SPDLOG_LOGGER_INFO(_logger, "skip() called while busy, ignoring");
        return;
    }
    _busy = true;
    getConnection();
}

void Skipper::test() const {
    _qeasy->test();
}

const ClashConfig &Skipper::config() const {
    return _qeasy->config();
}

void Skipper::setConfig(const ClashConfig &config) const {
    _qeasy->changeConfig(config);
}

void Skipper::getConnection() {
    const std::string url = _qeasy->config().connections();
    curl_easy_setopt(_qeasy->curl, CURLOPT_URL, url.c_str());
    connect(_qeasy, &QCurlEasy::done, this, &Skipper::handleGetConnectionThenKill, Qt::SingleShotConnection);
    _qeasy->perform();
}

void Skipper::handleGetConnectionThenKill(const QString &error, long code, const QByteArray &body) {
    const std::string url = _qeasy->config().connections();
    if (!error.isEmpty()) {
        SPDLOG_LOGGER_ERROR(_logger, "GET {} failed: {}", url, error.toStdString());
        _logger->flush();
        _busy = false;
        emit skipFinished(false);
        return;
    }
    if (code / 100 != 2) {
        SPDLOG_LOGGER_ERROR(_logger, "GET {} code={} body={}", url, code, body.toStdString());
        _logger->flush();
        _busy = false;
        emit skipFinished(false);
        return;
    }
    auto doc = QJsonDocument::fromJson(body);
    if (!doc["connections"].isArray()) {
        SPDLOG_LOGGER_WARN(_logger, "Failed to get connection, malformed json {}", body.toStdString());
        _logger->flush();
        _busy = false;
        emit skipFinished(false);
        return;
    }
    std::string connection_to_kill;
    for (auto obj : doc["connections"].toArray()) {
        // 部分 clash 内核（如启用 fake-ip 的 mihomo）会为所有连接填充 metadata.host，
        // 不能再用 host 是否为空来区分真正的对局连接和 API 请求。
        // 炉石传说的对局/登录连接固定走 actual.battle.net（端口 1119），
        // 而 api.blizzard.com 等域名只是非对局的 API 请求，需排除。
        if (obj.isObject() &&
            obj.toObject()["metadata"].toObject()["processPath"].toString().endsWith(
                "Hearthstone.app/Contents/MacOS/Hearthstone")) {
            const QString host = obj.toObject()["metadata"].toObject()["host"].toString();
            if (host.isEmpty() || host.contains("actual.battle.net")) {
                connection_to_kill = obj.toObject()["id"].toString().toStdString();
                SPDLOG_LOGGER_INFO(_logger, "Connection to kill {}", connection_to_kill);
                break;
            }
        }
    }
    if (connection_to_kill.empty()) {
        SPDLOG_LOGGER_WARN(_logger, "No connection to kill");
        _logger->flush();
        _busy = false;
        emit skipFinished(false);
        return;
    }
    const std::string url2 = _qeasy->config().kill_connection(connection_to_kill);
    curl_easy_setopt(_qeasy->curl, CURLOPT_URL, url2.c_str());
    curl_easy_setopt(_qeasy->curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    connect(_qeasy, &QCurlEasy::done, this, &Skipper::handleKillConnection, Qt::SingleShotConnection);
    _qeasy->perform();
}

void Skipper::handleKillConnection(const QString &error, long code, const QByteArray &body) {
    curl_easy_setopt(_qeasy->curl, CURLOPT_CUSTOMREQUEST, nullptr);
    curl_easy_setopt(_qeasy->curl, CURLOPT_HTTPGET, 1L);    // 恢复 CURL 的内部状态
    char *url;
    curl_easy_getinfo(_qeasy->curl, CURLINFO_EFFECTIVE_URL, &url);

    if (!error.isEmpty()) {
        SPDLOG_LOGGER_ERROR(_logger, "DELETE {} failed: {}", url, error.toStdString());
        _logger->flush();
        _busy = false;
        emit skipFinished(false);
        return;
    }
    if (code / 100 != 2) {
        SPDLOG_LOGGER_ERROR(_logger, "DELETE {} failed code={} body={}", url, code, body.toStdString());
        _logger->flush();
        _busy = false;
        emit skipFinished(false);
        return;
    }
    SPDLOG_LOGGER_INFO(_logger, "DELETE {}", url);
    
    // 立即释放 busy 状态并提示成功（使 UI 和反馈瞬间完成），同时在后台进行 5 秒后的二次断连以延长离线窗口
    _busy = false;
    emit skipFinished(true);

    QTimer::singleShot(5000, this, [this]() {
        const std::string url = _qeasy->config().connections();
        curl_easy_setopt(_qeasy->curl, CURLOPT_URL, url.c_str());
        connect(_qeasy, &QCurlEasy::done, this, &Skipper::killAnotherConnection, Qt::SingleShotConnection);
        _qeasy->perform();
    });
}

void Skipper::killAnotherConnection(const QString &error, long code, const QByteArray &body) {
    curl_easy_setopt(_qeasy->curl, CURLOPT_CUSTOMREQUEST, nullptr);
    curl_easy_setopt(_qeasy->curl, CURLOPT_HTTPGET, 1L);
    const std::string url = _qeasy->config().connections();
    if (!error.isEmpty() || code / 100 != 2) {
        _busy = false;
        emit skipFinished(false);
        return;
    }
    auto doc = QJsonDocument::fromJson(body);
    if (!doc["connections"].isArray()) {
        _busy = false;
        emit skipFinished(false);
        return;
    }
    std::string connection_to_kill;
    for (auto obj : doc["connections"].toArray()) {
        if (obj.isObject() &&
            obj.toObject()["metadata"].toObject()["processPath"].toString().endsWith(
                "Hearthstone.app/Contents/MacOS/Hearthstone")) {
            const QString host = obj.toObject()["metadata"].toObject()["host"].toString();
            if (host.isEmpty() || host.contains("actual.battle.net")) {
                connection_to_kill = obj.toObject()["id"].toString().toStdString();
                break;
            }
        }
    }
    if (connection_to_kill.empty()) {
        // 没有新连接了说明已经保持离线成功
        _busy = false;
        emit skipFinished(true);
        return;
    }
    const std::string url2 = _qeasy->config().kill_connection(connection_to_kill);
    curl_easy_setopt(_qeasy->curl, CURLOPT_URL, url2.c_str());
    curl_easy_setopt(_qeasy->curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    connect(_qeasy, &QCurlEasy::done, this, [this](const QString &error, long code, const QByteArray &body) {
        curl_easy_setopt(_qeasy->curl, CURLOPT_CUSTOMREQUEST, nullptr);
        curl_easy_setopt(_qeasy->curl, CURLOPT_HTTPGET, 1L);
        _busy = false;
        emit skipFinished(true);
    }, Qt::SingleShotConnection);
    _qeasy->perform();
}