/*
 * Strawberry Music Player
 * Copyright 2020-2021, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <QObject>
#include <QByteArray>
#include <QVariant>
#include <QString>
#include <QUrl>
#include <QUrlQuery>
#include <QRegularExpression>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonObject>

#include "core/shared_ptr.h"
#include "core/logging.h"
#include "core/networkaccessmanager.h"
#include "providers/musixmatchprovider.h"
#include "albumcoverfetcher.h"
#include "jsoncoverprovider.h"
#include "musixmatchcoverprovider.h"

MusixmatchCoverProvider::MusixmatchCoverProvider(Application *app, SharedPtr<NetworkAccessManager> network, QObject *parent)
    : JsonCoverProvider(QStringLiteral("Musixmatch"), true, false, 1.0, true, false, app, network, parent) {}

MusixmatchCoverProvider::~MusixmatchCoverProvider() {

  while (!replies_.isEmpty()) {
    QNetworkReply *reply = replies_.takeFirst();
    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
  }

}

bool MusixmatchCoverProvider::StartSearch(const QString &artist, const QString &album, const QString &title, const int id) {

  Q_UNUSED(title);

  if (artist.isEmpty() || album.isEmpty()) return false;

  QString artist_stripped = StringFixup(artist);
  QString album_stripped = StringFixup(album);

  if (artist_stripped.isEmpty() || album_stripped.isEmpty()) return false;

  QUrl url(QStringLiteral("https://www.musixmatch.com/album/%1/%2").arg(artist_stripped, album_stripped));
  QNetworkRequest req(url);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  QNetworkReply *reply = network_->get(req);
  replies_ << reply;
  QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, id, artist, album]() { HandleSearchReply(reply, id, artist, album); });

  //qLog(Debug) << "Musixmatch: Sending request for" << artist_stripped << album_stripped << url;

  return true;

}

void MusixmatchCoverProvider::CancelSearch(const int id) { Q_UNUSED(id); }

void MusixmatchCoverProvider::HandleSearchReply(QNetworkReply *reply, const int id, const QString &artist, const QString &album) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  CoverProviderSearchResults results;

  if (reply->error() != QNetworkReply::NoError) {
    Error(QStringLiteral("%1 (%2)").arg(reply->errorString()).arg(reply->error()));
    emit SearchFinished(id, results);
    return;
  }
  else if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
    Error(QStringLiteral("Received HTTP code %1").arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()));
    emit SearchFinished(id, results);
    return;
  }

  const QByteArray data = reply->readAll();
  if (data.isEmpty()) {
    Error(QStringLiteral("Empty reply received from server."));
    emit SearchFinished(id, results);
    return;
  }
  const QString content = QString::fromUtf8(data);
  const QString data_begin = QLatin1String("<script id=\"__NEXT_DATA__\" type=\"application/json\">");
  const QString data_end = QLatin1String("</script>");
  if (!content.contains(data_begin) || !content.contains(data_end)) {
    emit SearchFinished(id, results);
    return;
  }
  qint64 begin_idx = content.indexOf(data_begin);
  QString content_json;
  if (begin_idx > 0) {
    begin_idx += data_begin.length();
    qint64 end_idx = content.indexOf(data_end, begin_idx);
    if (end_idx > begin_idx) {
      content_json = content.mid(begin_idx, end_idx - begin_idx);
    }
  }

  if (content_json.isEmpty()) {
    emit SearchFinished(id, results);
    return;
  }

  if (content_json.contains(QRegularExpression(QStringLiteral("<[^>]*>")))) {  // Make sure it's not HTML code.
    emit SearchFinished(id, results);
    return;
  }

  QJsonParseError error;
  QJsonDocument json_doc = QJsonDocument::fromJson(content_json.toUtf8(), &error);

  if (error.error != QJsonParseError::NoError) {
    Error(QStringLiteral("Failed to parse json data: %1").arg(error.errorString()));
    emit SearchFinished(id, results);
    return;
  }

  if (json_doc.isEmpty()) {
    Error(QStringLiteral("Received empty Json document."), data);
    emit SearchFinished(id, results);
    return;
  }

  if (!json_doc.isObject()) {
    Error(QStringLiteral("Json document is not an object."), json_doc);
    emit SearchFinished(id, results);
    return;
  }

  QJsonObject obj_data = json_doc.object();
  if (obj_data.isEmpty()) {
    Error(QStringLiteral("Received empty Json object."), json_doc);
    emit SearchFinished(id, results);
    return;
  }

  if (!obj_data.contains(QLatin1String("props")) || !obj_data[QLatin1String("props")].isObject()) {
    Error(QStringLiteral("Json reply is missing props."), obj_data);
    emit SearchFinished(id, results);
    return;
  }
  obj_data = obj_data[QLatin1String("props")].toObject();

  if (!obj_data.contains(QLatin1String("pageProps")) || !obj_data[QLatin1String("pageProps")].isObject()) {
    Error(QStringLiteral("Json props is missing pageProps."), obj_data);
    emit SearchFinished(id, results);
    return;
  }
  obj_data = obj_data[QLatin1String("pageProps")].toObject();

  if (!obj_data.contains(QLatin1String("data")) || !obj_data[QLatin1String("data")].isObject()) {
    Error(QStringLiteral("Json pageProps is missing data."), obj_data);
    emit SearchFinished(id, results);
    return;
  }
  obj_data = obj_data[QLatin1String("data")].toObject();

  if (!obj_data.contains(QLatin1String("albumGet")) || !obj_data[QLatin1String("albumGet")].isObject()) {
    Error(QStringLiteral("Json data is missing albumGet."), obj_data);
    emit SearchFinished(id, results);
    return;
  }
  obj_data = obj_data[QLatin1String("albumGet")].toObject();

  if (!obj_data.contains(QLatin1String("data")) || !obj_data[QLatin1String("data")].isObject()) {
    Error(QStringLiteral("Json albumGet reply is missing data."), obj_data);
    emit SearchFinished(id, results);
    return;
  }
  obj_data = obj_data[QLatin1String("data")].toObject();

  CoverProviderSearchResult result;
  if (obj_data.contains(QLatin1String("artistName")) && obj_data[QLatin1String("artistName")].isString()) {
    result.artist = obj_data[QLatin1String("artistName")].toString();
  }
  if (obj_data.contains(QLatin1String("name")) && obj_data[QLatin1String("name")].isString()) {
    result.album = obj_data[QLatin1String("name")].toString();
  }

  if (result.artist.compare(artist, Qt::CaseInsensitive) != 0 && result.album.compare(album, Qt::CaseInsensitive) != 0) {
    emit SearchFinished(id, results);
    return;
  }

  QList<QPair<QString, QSize>> cover_sizes = QList<QPair<QString, QSize>>() << qMakePair(QStringLiteral("coverImage800x800"), QSize(800, 800))
                                                                            << qMakePair(QStringLiteral("coverImage500x500"), QSize(500, 500))
                                                                            << qMakePair(QStringLiteral("coverImage350x350"), QSize(350, 350));

  for (const QPair<QString, QSize> &cover_size : cover_sizes) {
    if (!obj_data.contains(cover_size.first)) continue;
    QUrl cover_url(obj_data[cover_size.first].toString());
    if (cover_url.isValid()) {
      result.image_url = cover_url;
      result.image_size = cover_size.second;
      results << result;
    }
  }

  emit SearchFinished(id, results);

}

void MusixmatchCoverProvider::Error(const QString &error, const QVariant &debug) {

  qLog(Error) << "Musixmatch:" << error;
  if (debug.isValid()) qLog(Debug) << debug;

}
