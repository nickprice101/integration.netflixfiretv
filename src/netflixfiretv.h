/******************************************************************************
 *
 * Copyright (C) 2019 Marton Borzak <hello@martonborzak.com>
 *
 * This file is part of the YIO-Remote software project.
 *
 * YIO-Remote software is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * YIO-Remote software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with YIO-Remote software. If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *****************************************************************************/

#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>

#include "yio-interface/entities/mediaplayerinterface.h"
#include "yio-model/mediaplayer/albummodel_mediaplayer.h"
#include "yio-model/mediaplayer/searchmodel_mediaplayer.h"
#include "yio-model/mediaplayer/speakermodel_mediaplayer.h"
#include "yio-plugin/integration.h"
#include "yio-plugin/plugin.h"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//// NETFLIXFIRETV FACTORY
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const bool USE_WORKER_THREAD = false;

class NetflixFireTvPlugin : public Plugin {
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "YIO.PluginInterface" FILE "netflixfiretv.json")

 public:
     NetflixFireTvPlugin();

    // Plugin interface
 protected:
    Integration* createIntegration(const QVariantMap& config, EntitiesInterface* entities,
                                   NotificationsInterface* notifications, YioAPIInterface* api,
                                   ConfigInterface* configObj) override;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//// NETFLIXFIRETV CLASS
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class  NetflixFireTv : public Integration {
    Q_OBJECT

 public:
    explicit  NetflixFireTv(const QVariantMap& config, EntitiesInterface* entities, NotificationsInterface* notifications,
                     YioAPIInterface* api, ConfigInterface* configObj, Plugin* plugin);

    void sendCommand(const QString& type, const QString& entitId, int command, const QVariant& param) override;

 public slots:  // NOLINT open issue: https://github.com/cpplint/cpplint/pull/99
    void connect() override;
    void disconnect() override;
    void enterStandby() override;
    void leaveStandby() override;

 signals:
    void requestReady(const QVariantMap& obj, const QString& url);
    void headersReady(const QVariantMap& obj); //const QString& id, const QString& title, const QString& subtitle, const QString& imgUrl);

 private:
    //  NetflixFireTv API calls
    void search(QString query);
    void search(QString query, QString type);
    void getAlbum(QString id);
    void getPlaylist(QString id);
    void getUserPlaylists();

    //  NetflixFireTv status adb calls
    bool adbConnect(const QString& ip);
    void getCurrentPlayer();
    static QString sendAdbCommand(const QString& message);
    //QByteArray sendAdbCommand_old(const QString& message);
    void parseRecent(BrowseModel* recentModel); // parse recently viewed content. Pass the model through as we iterate.
    static bool netflixActive();
    static bool openNetflix(); // get focus for Nettflix

    void updateEntity(const QString& entity_id, const QVariantMap& attr);

    // get and post requests
    void getRequest(const QString& url, const QString& params);  // TODO(marton): change param to QUrlQuery
                                                                 // QUrlQuery query;

    // speaker/source selection
    void changeDevice(QString id);  //change the speaker/source
    void getDevices();

    // general functions
    QString convertSE(int series, int episode);
    QString getCountryId(const QString& countryCode);
    QString getHead(const QString& theWebPAge);

 private slots:  // NOLINT open issue: https://github.com/cpplint/cpplint/pull/99
    void onPollingTimerTimeout();
    void getDirect(QNetworkReply * reply);

 private:
    QString m_entityId;

    // polling timer
    QTimer* m_pollingTimer;

    // Really bad
    QString m_recentMessage = "";

    // ADB details
    QString m_serverAddress;
    QString m_firetvAddress = "";
    QStringList m_firetvDevices; // all devices
    bool m_adbConnect = false; // are we connected to the fire tv.
    bool m_newShow = true; // update only when the show changes.
    QString m_currentShow; // currently playing show

    //Fire TV status
    int m_firetvVol = 100; //track volume, default to max

    // Netflix unoffical API auth
    QString m_apiUrl = "unogsng.p.rapidapi.com";
    QString m_apiUrl2 = "unogs-unogs-v1.p.rapidapi.com";
    QString m_apiToken;
    QString m_apiCountry;

    QVector<QStringList> m_countryTable{{"AU","BR","CA","FR","DE","GR","HK","IS","IN","IT","JP","NL","SK","KR","ES","SE","GB","US"},{"23","29","33","45","39","327","331","265","337","269","267","67","412","348","270","73","46","78"}};
    QStringList m_recentShows;
};
