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

#include "netflixfiretv.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <QProcess>
#include "adbclient.h"


NetflixFireTvPlugin::NetflixFireTvPlugin() : Plugin("netflixfiretv", USE_WORKER_THREAD) {}

Integration* NetflixFireTvPlugin::createIntegration(const QVariantMap& config, EntitiesInterface* entities,
                                              NotificationsInterface* notifications, YioAPIInterface* api,
                                              ConfigInterface* configObj) {
    qCInfo(m_logCategory) << "Creating NetflixFireTv integration plugin" << PLUGIN_VERSION;

    return new NetflixFireTv(config, entities, notifications, api, configObj, this);
}

NetflixFireTv::NetflixFireTv(const QVariantMap& config, EntitiesInterface* entities, NotificationsInterface* notifications,
                 YioAPIInterface* api, ConfigInterface* configObj, Plugin* plugin)
    : Integration(config, entities, notifications, api, configObj, plugin) {
    for (QVariantMap::const_iterator iter = config.begin(); iter != config.end(); ++iter) {
        if (iter.key() == Integration::OBJ_DATA) {
            QVariantMap map = iter.value().toMap();
            m_entityId        = map.value("entity_id").toString();
            m_serverAddress   = map.value("adb_server_address").toString();
            m_firetvDevices   = map.value("firetv_address_list").toString().split(",");
            m_apiToken        = map.value("api_token").toString();
            m_apiCountry      = map.value("netflix_country_code").toString();
        }
    }

    m_pollingTimer = new QTimer(this);
    m_pollingTimer->setInterval(4000);
    QObject::connect(m_pollingTimer, &QTimer::timeout, this, &NetflixFireTv::onPollingTimerTimeout);

    // add available entity
    QStringList supportedFeatures;
    supportedFeatures << "SOURCE"
                      << "APP_NAME"
                      << "VOLUME"
                      << "VOLUME_UP"
                      << "VOLUME_DOWN"
                      << "VOLUME_SET"
                      << "MUTE"
                      << "MUTE_SET"
                      << "MEDIA_TYPE"
                      << "MEDIA_TITLE"
                      << "MEDIA_ARTIST"
                      << "MEDIA_ALBUM"
                      << "MEDIA_DURATION"
                      << "MEDIA_POSITION"
                      << "MEDIA_IMAGE"
                      << "PLAY"
                      << "PAUSE"
                      << "STOP"
                      << "PREVIOUS"
                      << "NEXT"
                      << "SEEK"
                      << "SHUFFLE"
                      << "SEARCH"
                      << "SPEAKER_CONTROL"
                      << "LIST";
    addAvailableEntity(m_entityId, "media_player", integrationId(), friendlyName(), supportedFeatures);
}

void NetflixFireTv::connect() {
    qCDebug(m_logCategory) << "STARTING NETFLIXFIRETV";
    setState(CONNECTED);

    if (m_firetvAddress.isEmpty()) { m_firetvAddress = m_firetvDevices[0]; } // set to first entry if nothing is defined yet.

    // check for api key
    if (m_apiToken.isNull() || m_apiToken.isEmpty()) {
        qCWarning(m_logCategory) << "No api key provided!";
        return;
    }

    // check we're connected to the firetv
    if (!m_adbConnect) {
        qCDebug(m_logCategory) << "Not connected to Fire TV. Connecting...";
        if (!adbConnect(m_firetvAddress)) { qCDebug(m_logCategory) << "Cannot connect to adb device: " << m_firetvAddress; }
    }

    // start polling
    //m_pollingTimer->start();
}

void NetflixFireTv::disconnect() {
    setState(DISCONNECTED);
    m_pollingTimer->stop();
    m_adbConnect = false; // reset connection flag so we check again on restart.
}

void NetflixFireTv::enterStandby() { disconnect(); } // stop polling on disconnect

void NetflixFireTv::leaveStandby() { connect(); }

bool NetflixFireTv::adbConnect(const QString& ip) {
    AdbClient *adb = new AdbClient(m_serverAddress); // initialise
    QString cmdLine;
    QString result;

    //if (m_adbConnect) {
        cmdLine = "host:disconnect"; //disconnect everything as I haven't figured out how to use the -s parameter to select the target.
        result = adb->doAdbCommands(cmdLine.toUtf8().constData());
        qCDebug(m_logCategory) << "ADB disconnect response: " << result;
    //}

    cmdLine = "host:connect:" + ip;
    result = adb->doAdbCommands(cmdLine.toUtf8().constData());
    qCDebug(m_logCategory) << "ADB connect response: " << result;

    //qCDebug(m_logCategory) << "ADB devices response: " << adb->doAdbCommands("host:devices");
    if (result.contains("fail")) {
        m_adbConnect = false;
        m_notifications->add(true,tr("Cannot connect to device. Ensure ADB Debugging is enabled."));
        cmdLine = "host:disconnect:" + ip; // if connect failed then make sure we disconnect.
        result = adb->doAdbCommands(cmdLine.toUtf8().constData());
    } else {
        m_firetvAddress = ip;
        m_adbConnect = true; }
    delete adb;
    return m_adbConnect;
}

void NetflixFireTv::search(QString query) { search(query, ""); } // search all
void NetflixFireTv::search(QString query, QString type) {
    QString url = "https://" + m_apiUrl + "/search";

    query.replace(" ", "%20");

    QObject* context = new QObject(this);
    QObject::connect(this, &NetflixFireTv::requestReady, context, [=](const QVariantMap& map, const QString& rUrl) {
        if (rUrl == url) {  //parse the search response
            if (map.contains("results")) {
                //create the response groupings
                SearchModelList* movies = new SearchModelList();
                SearchModelList* shows = new SearchModelList();

                QString itemType;
                QString id;
                QString title;
                QString subtitle;
                QString image;
                QStringList commands;

                QVariantList results = map.value("results").toList();
                for (int i = 0; i < results.length(); i++) {
                    id = results[i].toMap().value("nfid").toString();
                    title = results[i].toMap().value("title").toString().replace("&#39;","'") + "(" + results[i].toMap().value("year").toString() + ")";
                    subtitle = results[i].toMap().value("synopsis").toString().left(50).replace("&#39;","'");

                    if (results[i].toMap().value("vtype").toString() == "series") { itemType = "show";
                    } else if (results[i].toMap().value("vtype").toString() == "movie") { itemType = "movie"; }

                    QStringList commands = {"PLAY"};
                    image = results[i].toMap().value("img").toString();

                    SearchModelListItem item = SearchModelListItem(id, itemType, title, subtitle, image, commands);
                    if (itemType == "movie") {           movies->append(item);
                    } else if (itemType == "show") {     shows->append(item); }
                }

                //change search items based on content
                SearchModelItem* imovies    = new SearchModelItem("movies",movies);
                SearchModelItem* ishows     = new SearchModelItem("shows", shows);

                SearchModel* netflixResults = new SearchModel();

                netflixResults->append(imovies);
                netflixResults->append(ishows);

                // update the entity
                EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
                if (entity) {
                    MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                    me->setSearchModel(netflixResults);
                }
            }
        }
        context->deleteLater();
    });

    //convert type to integer
    QString newType;
    if (type.contains("movies")) {                           newType = "&type=movies"; }
    if (type.contains("shows")) {                            newType = "&type=series"; }
    if (type.contains("movies") && type.contains("shows")) { newType = ""; }

    QString message = "?query=" + query + newType + "&country_andorunique=and&countrylist=" + getCountryId(m_apiCountry) + "&orderby=date&limit=30";
    getRequest(url, message);
}

void NetflixFireTv::getAlbum(QString id) {
    QString url = "https://" + m_apiUrl + "/episodes";
    QString message = "?netflixid=" + id;
    qCDebug(m_logCategory) << "GET SHOW CALLED. SENDING TO: " << url << message;

    QObject* context = new QObject(this);
    QObject::connect(this, &NetflixFireTv::requestReady, context, [=](const QVariantMap& map, const QString& rUrl) {
        if (rUrl == url) {
            qCDebug(m_logCategory) << "GET SHOW";
            if (map.contains("data")) { qCDebug(m_logCategory) << "contains data"; }
            if (map.contains("episode")) { qCDebug(m_logCategory) << "contains episode"; }
            if (map.value("data").toMap().contains("episode")) { qCDebug(m_logCategory) << "contains data and then episode"; }
            qCDebug(m_logCategory) << "map size is: " << map.size();
            qCDebug(m_logCategory) << "data length is: " << map.value("data").toList().length();

            //alternative - create global array of search or playlist results and loop back through to find the show selected?
            QString title = "";
            QString subtitle = "";
            QString type = "episode";
            QString image = map.value("data").toList()[0].toMap().value("episodes").toList()[0].toMap().value("img").toString(); //use image for first episode
            QStringList commands = {"PLAY"};
            BrowseModel* album = new BrowseModel(nullptr,
                                                map.value("data").toList()[0].toMap().value("episodes").toList()[0].toMap().value("epid").toString(),
                                                title,
                                                subtitle,
                                                type,
                                                "show",
                                                commands);
            qCDebug(m_logCategory) << "Browse model initiated";
            QVariantList seasons = map.value("data").toList();
            for (int i = 0; i < seasons.length(); i++) { // loop through the seasons
                qCDebug(m_logCategory) << "1st loop begins";
                QVariantList episodes = seasons[i].toMap().value("episodes").toList();
                for (int j = 0; j < episodes.length(); j++) { // loop through the current season
                     album->addItem(episodes[j].toMap().value("epid").toString(),
                                  convertSE(episodes[j].toMap().value("seasnum").toInt(),episodes[j].toMap().value("epnum").toInt()) + episodes[j].toMap().value("title").toString().replace("&#39;","'"),
                                  episodes[j].toMap().value("synopsis").toString().left(50).replace("&#39;","'"),
                                  type,
                                  episodes[j].toMap().value("img").toString(),
                                  commands);
                }
            }

            // update the entity
            EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
            if (entity) {
                MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                me->setBrowseModel(album);
            }
        }
        context->deleteLater();
    });
    getRequest(url, message);
}

void NetflixFireTv::getPlaylist(QString id) {
    QString url = "https://" + m_apiUrl + "/search";
    QString message;
    QString genres;
    QString listTitle = "";
    QString listSubtitle = "";
    QString listImage = "";

    if (id == "adb_recent") {
        BrowseModel* recentModel = new BrowseModel(nullptr, "adb_recent", "Recently Viewed", "", "show", "qrc:/images/netflix_recent.png", {"PLAY"});
        QString result = sendAdbCommand("pm dump com.netflix.ninja | grep netflix://title/");
        m_recentShows = result.split("\n");
        m_recentShows.removeDuplicates();
        parseRecent(recentModel);
        return;
    } else if (id == "sch_comedy") {
        genres = "1009,1402,2700,3903,4426,4906";
        listTitle = "Latest Comedy";
        listSubtitle = "Latest comedy releases";
        listImage = "qrc:/images/netflix_comedy.png";
    } else if (id == "sch_standup") {
        genres = "10778";
        listTitle = "Latest Stand-up";
        listSubtitle = "Latest stand-up releases";
        listImage = "qrc:/images/netflix_standup.png";
    } else if (id == "sch_drama") {
        genres = "5763,2748,3179,3682,3916,3947";
        listTitle = "Latest Drama";
        listSubtitle = "Latest drama releases";
        listImage = "qrc:/images/netflix_drama.png";
    } else if (id == "sch_action") {
        genres = "899,1568,1492,1694,3327,3916";
        listTitle = "Latest Action";
        listSubtitle = "Latest action releases";
        listImage = "qrc:/images/netflix_action.png";
    }
    message = "?country_andorunique=and&countrylist=" + getCountryId(m_apiCountry) + "&genrelist=" + genres + "&type=series&orderby=date&limit=30";

    if (id == "cgi_release") {
        url = "https://" + m_apiUrl2 + "/api.cgi";
        listTitle = "Latest Releases";
        listSubtitle = "Latest releases";
        listImage = "qrc:/images/netflix_releases.png";
        message = "?q=get:new14:" + m_apiCountry + "&p=1&t=ns&st=adv"; // new7 = last 7 days, p = page (per 100)
    } else if (id == "cgi_season") {
        url = "https://" + m_apiUrl2 + "/api.cgi";
        listTitle = "New Seasons";
        listSubtitle = "Latest new seasons added";
        listImage = "qrc:/images/netflix_seasons.png";
        message = "?q=get:seasons14:" + m_apiCountry + "&p=1&t=ns&st=adv";
    } else if (id == "cgi_last") {
        url = "https://" + m_apiUrl2 + "/api.cgi";
        listTitle = "Last Chance";
        listSubtitle = "Last chance to watch";
        listImage = "qrc:/images/netflix_lastchance.png";
        message = "?q=get:exp:" + m_apiCountry + "&p=1&t=ns&st=adv";
    } else if (id == "sch_movies") {
        listTitle = "Latest Movies";
        listSubtitle = "Latest movies";
        listImage = "qrc:/images/netflix_movies.png";
        message = "?country_andorunique=and&countrylist=" + getCountryId(m_apiCountry) + "&type=movie&orderby=date&limit=30";
    }

    QObject* context = new QObject(this);
    QObject::connect(this, &NetflixFireTv::requestReady, context, [=](const QVariantMap& map, const QString& rUrl) {
        if (rUrl == url) {
            if (rUrl.contains("/search")) {
                qCDebug(m_logCategory) << "GET SHOW /search";
                QVariantList shows = map.value("results").toList();
                QString title = listTitle;
                QString subtitle = listSubtitle;
                QString type = "episode";
                QString image = listImage; //use image for the first show?
                QStringList commands = {"PLAY"};
                BrowseModel*  album = new BrowseModel(nullptr,
                                                      shows[0].toMap().value("epid").toString(),
                                                      title,
                                                      subtitle,
                                                      type,
                                                      image,
                                                      {""});

                for (int i = 0; i < shows.length(); i++) { // loop through the current shows
                    album->addItem(shows[i].toMap().value("nfpid").toString(),
                                   shows[i].toMap().value("title").toString().replace("&#39;","'") + " (" + shows[i].toMap().value("year").toString() + ")",
                                   shows[i].toMap().value("synopsis").toString().left(50).replace("&#39;","'"),
                                   type,
                                   shows[i].toMap().value("img").toString(),
                                   commands);
                }

                // update the entity
                EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
                if (entity) {
                    MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                    me->setBrowseModel(album);
                }

            } else {
                qCDebug(m_logCategory) << "GET SHOW /api.cgi";
                QVariantList shows = map.value("ITEMS").toList();
                QString title = listTitle;
                QString subtitle = listSubtitle;
                QString type = "episode";
                QString image = listImage;
                //QString image = shows[0].toStringList().at(2); //use image for the first show
                QStringList commands = {"PLAY"};
                BrowseModel*  album = new BrowseModel(nullptr,
                                                      "/title/" + shows[0].toStringList().at(0),
                                                      title,
                                                      subtitle,
                                                      type,
                                                      image,
                                                      {""});
                for (int i = 0; i < shows.length(); i++) { // loop through the current shows
                    title = shows[i].toStringList().at(1);
                    subtitle = shows[i].toStringList().at(3);
                    album->addItem("/title/" + shows[i].toStringList().at(0),
                                   title.replace("&#39;","'") + " (" + shows[i].toStringList().at(7) + ")",
                                   subtitle.left(50).replace("&#39;","'"),
                                   type,
                                   shows[i].toStringList().at(2),
                                   commands);
                }

                // update the entity
                EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
                if (entity) {
                    MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                    me->setBrowseModel(album);
                }
            }

        }
        context->deleteLater();
    });
    getRequest(url, message);
}

void NetflixFireTv::getUserPlaylists() {
    qCDebug(m_logCategory) << "ADD PREDEFINED PLAYLISTS";
    QString     id       = "na";
    QString     title    = "User Playlists";
    QString     subtitle = "Pre-defined user playlists";
    QString     type     = "playlist";
    QString     image    = "";
    QStringList commands = {"PLAY"};

    BrowseModel* album = new BrowseModel(nullptr, id, title, subtitle, type, image, commands);
    //album->setObjectName("userplaylists");

    album->addItem("adb_recent","Recently Viewed","Recently viewed shows",type,"qrc:/images/netflix_recent.png",commands);
    album->addItem("cgi_release","New Releases","New Releases in your country",type,"qrc:/images/netflix_releases.png",commands);
    album->addItem("cgi_season","New Seasons","New Releases in your country",type,"qrc:/images/netflix_seasons.png",commands);
    album->addItem("cgi_last","Last Chance","Last chance to view these shows",type,"qrc:/images/netflix_lastchance.png",commands);
    album->addItem("sch_comedy","Recent Comedy","Recent comedy releases",type,"qrc:/images/netflix_comedy.png",commands);
    album->addItem("sch_standup","Recent Stand-up","Recent stand-up releases", type,"qrc:/images/netflix_standup.png",commands);
    album->addItem("sch_drama","Recent Drama","Recent drama releases",type,"qrc:/images/netflix_drama.png",commands);
    album->addItem("sch_action","Recent Thriller","Recent thriller releases",type,"qrc:/images/netflix_thriller.png",commands);
    album->addItem("sch_movies","Recent Movies","Recent movie releases",type,"qrc:/images/netflix_movies.png",commands);

    // update the entity
    EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
    if (entity) {
        MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
        me->setBrowseModel(album);
        qCDebug(m_logCategory) << entity->getSpecificInterface();
    }
}

void NetflixFireTv::getCurrentPlayer() {

    EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
    if (entity && netflixActive()) { // only update if netflix is the active player
        // MAKE ADB CALL AND CHECK PLAYER STATUS
        QString result = sendAdbCommand("adb shell dumpsys window windows | grep -E 'mCurrentFocus|mFocusedApp'");



        // reduce the burden if track/show/movie hasn't changed.
        if (m_newShow) {

            // get the image. work backwards depending on the metadata available.
            QString image = "";
            entity->updateAttrByIndex(MediaPlayerDef::MEDIAIMAGE, image);

            // get the device
            entity->updateAttrByIndex(MediaPlayerDef::SOURCE,
                                      "Fire TV");

            // get the episode title
            entity->updateAttrByIndex(MediaPlayerDef::MEDIATITLE,
                                      "The title");

            // get the show/movie title
            entity->updateAttrByIndex(MediaPlayerDef::MEDIAARTIST,
                                      "The subtitle");
        }

        // get the state
        //if (STATUS == "playing") {
        //    entity->updateAttrByIndex(MediaPlayerDef::STATE, MediaPlayerDef::PLAYING);
        //} else {
        //    entity->updateAttrByIndex(MediaPlayerDef::STATE, MediaPlayerDef::IDLE);
        //}

        // update progress
        entity->updateAttrByIndex(
            MediaPlayerDef::MEDIADURATION,
            static_cast<int>(1000 / 1000));
        entity->updateAttrByIndex(MediaPlayerDef::MEDIAPROGRESS,
                                  static_cast<int>(500 / 1000));

    } else { // if no players then empty the player screen.
        qCDebug(m_logCategory) << "No players discovered. Clearing player.";
        entity->updateAttrByIndex(MediaPlayerDef::MEDIAIMAGE, "");
        entity->updateAttrByIndex(MediaPlayerDef::SOURCE, "");
        entity->updateAttrByIndex(MediaPlayerDef::MEDIATITLE, "");
        entity->updateAttrByIndex(MediaPlayerDef::MEDIAARTIST, "");
        entity->updateAttrByIndex(MediaPlayerDef::MEDIADURATION, 0);
        entity->updateAttrByIndex(MediaPlayerDef::MEDIAPROGRESS, 0);
        entity->updateAttrByIndex(MediaPlayerDef::STATE, MediaPlayerDef::OFF);
    }
}

void NetflixFireTv::sendCommand(const QString& type, const QString& entityId, int command, const QVariant& param) {
    if (!(type == "media_player" && entityId == m_entityId)) { return; }

    if (!m_adbConnect) {
        qCWarning(m_logCategory) << "Not connected to Fire Tv!";
        if (!adbConnect(m_firetvAddress)) { return; }
    }

    if (command == MediaPlayerDef::C_PLAY) {
        sendAdbCommand("input key event 126");  // normal play without browsing
    } else if (command == MediaPlayerDef::C_PLAY_ITEM) {
        if (param == "") {
            sendAdbCommand("input key event 126");  // normal play without browsing
        } else {
            if (param.toMap().contains("type")) {
                //QString message = "am start -a android.intent.action.VIEW -d http://www.netflix.com/" + param.toMap().value("id").toString();
                //QString message = "am start -n com.netflix.ninja/.ui.launch.UIWebViewActivity -a android.intent.action.ACTION_VIEW -d http://www.netflix.com/" + param.toMap().value("id").toString(); // use watch/id to play the item.
                QString message = "am start -a android.intent.action.ACTION_VIEW -d http://www.netflix.com/" + param.toMap().value("id").toString(); // use watch/id to play the item.
                QString result = sendAdbCommand(message); //parse result for user feedback?
            }
        }
    } else if (command == MediaPlayerDef::C_PAUSE) {
        sendAdbCommand("input key event 127");
    } else if (command == MediaPlayerDef::C_NEXT) {
        sendAdbCommand("input key event 87"); // make next do a scrub?
        m_newShow = true; // this would be picked up by the polling but better to pre-empt it and speed everything up a bit.
    } else if (command == MediaPlayerDef::C_PREVIOUS) {
        sendAdbCommand("input key event 88");
        m_newShow = true; // as above
    } else if (command == MediaPlayerDef::C_SEARCH) {
        qCDebug(m_logCategory) << "Search submitted";
        search(param.toString());
    } else if (command == MediaPlayerDef::C_GETALBUM) {
        qCDebug(m_logCategory) << "GET SHOW ACTION INVOKED. TYPE = " << param.toString();
        getAlbum(param.toString());
    } else if (command == MediaPlayerDef::C_GETPLAYLIST) {
        qCDebug(m_logCategory) << "PLAYLIST ACTION INVOKED. TYPE = " << param.toString();
        if (param.toString() == "user") { // add in season check for alternative view?
            getUserPlaylists();
        } else {
            getPlaylist(param.toString());
        }
    } else if (command == MediaPlayerDef::C_CHANGE_SPEAKER) {
        changeDevice(param.toString());
    } else if (command == MediaPlayerDef::C_GET_SPEAKERS) {
        getDevices();
    } else if (command == MediaPlayerDef::C_CURSOR_UP) {
        sendAdbCommand("input key event 19");
    } else if (command == MediaPlayerDef::C_CURSOR_DOWN) {
        sendAdbCommand("input key event 20");
    } else if (command == MediaPlayerDef::C_CURSOR_LEFT) {
        sendAdbCommand("input key event 21");
    } else if (command == MediaPlayerDef::C_CURSOR_RIGHT) {
        sendAdbCommand("input key event 22");
    } else if (command == MediaPlayerDef::C_CURSOR_OK) {
        sendAdbCommand("input key event 23");
    }

}

void NetflixFireTv::changeDevice(QString id) {
    if (id != m_firetvAddress) {
        adbConnect(id);
        getDevices(); // refresh the model.
    }
}

void NetflixFireTv::getDevices() {
    qCDebug(m_logCategory) << "GET DEVICES";
    QString     id          = "root";
    QString     name        = "Devices";
    QString     description = "User-defined devices";
    QString     type        = "device";
    QString     image       = "";
    QStringList commands    = {"CONNECT"};
    QStringList supported   = {};

    SpeakerModel* devices = new SpeakerModel(nullptr, id, name, description, type, image, commands, supported);

    for (int i = 0; i < m_firetvDevices.count(); i++) {
        // add in adb call to get more information about the device? I.e. name, what is (or is it) active?
        if (m_firetvAddress == m_firetvDevices[i]) {
            devices->addItem(m_firetvDevices[i],m_firetvDevices[i],"Active connection",type,image,{""},supported);
        } else {
            devices->addItem(m_firetvDevices[i],m_firetvDevices[i],"Not connected",type,image,commands,supported);
        }
    }

    //emit devices->speakerModelChanged(); model.index(i).data(model.NameRole)
    // update the entity
    EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
    if (entity) {
        qCDebug(m_logCategory) << "Entity pointer: " << entity;
        MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
        me->setSpeakerModel(devices);
    }
}

QString NetflixFireTv::sendAdbCommand(const QString& message) {
    //adbConnect(m_firetvAddress);
    AdbClient *adb = new AdbClient();
    QString result = adb->doAdbShell(message);
    delete adb;
    return result;
}

//QByteArray NetflixFireTv::sendAdbCommand_old(const QString& message) {
//    qCDebug(m_logCategory) << "SENDING ADB COMMAND: " << message;
//    QProcess adbCommand;

//    adbCommand.setProcessChannelMode(QProcess::MergedChannels);
//    adbCommand.start("/usr/bin/ssh sshd@192.168.1.50 \"docker exec adb-server adb " + message + "\"");
//    adbCommand.waitForReadyRead(2000); // timeout of two seconds
//    QByteArray output = adbCommand.readAll();
//    adbCommand.waitForFinished(2000); // wait for command to finish, max wait time is 2 seconds

//    qCDebug(m_logCategory) << "ADB COMMAND RESPONSE: " << output;
//    return output;
//}

void NetflixFireTv::getRequest(const QString& url, const QString& params) {
    // create new networkacces manager and request
    QNetworkAccessManager* manager = new QNetworkAccessManager(this);
    QNetworkRequest        request;

    QObject* context = new QObject(this);

    // connect to finish signal
    QObject::connect(manager, &QNetworkAccessManager::finished, context, [=](QNetworkReply* reply) {
        if (reply->error()) {
            qCWarning(m_logCategory) << reply->errorString();
        }

        QString     answer = reply->readAll();
        //qCDebug(m_logCategory) << "Response from GET: " << answer;

        if (answer != "") {
            if (answer.left(1) == "[") { answer = "{\n \"data\": " + answer + "\n }"; } //cheap way of converting from JsonArray to JsonObject
            qCDebug(m_logCategory) << "Response from GET: " << answer;
            QMap<QString, QVariant> map;
            // convert to json
            QJsonParseError parseerror;
            QJsonDocument   doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);
            if (parseerror.error != QJsonParseError::NoError) {
                qCWarning(m_logCategory) << "JSON error : " << parseerror.errorString();
                return;
            }
            // create a map object
            map = doc.toVariant().toMap();
            emit requestReady(map, url);
        }

        reply->deleteLater();
        context->deleteLater();
        manager->deleteLater();
    });

    QObject::connect(
        manager, &QNetworkAccessManager::networkAccessibleChanged, context,
        [=](QNetworkAccessManager::NetworkAccessibility accessibility) { qCDebug(m_logCategory) << accessibility; });

    // set headers
    request.setRawHeader("Accept", "application/json");
    QString host = url.mid(8,url.indexOf(".com") - 4); // + 4 - 8
    qCDebug(m_logCategory) << "Setting x-rapidapi-host to: " << host;
    request.setRawHeader("x-rapidapi-host", host.toLocal8Bit());
    request.setRawHeader("x-rapidapi-key", m_apiToken.toLocal8Bit());
    request.setRawHeader("useQueryString", "true");

    // set the URL
    request.setUrl(QUrl::fromUserInput(url + params));

    qCDebug(m_logCategory) << "Sending as GET: " + request.url().toString();

    // send the get request
    manager->get(request);
}

void NetflixFireTv::parseRecent(BrowseModel* recentModel) {
    qCDebug(m_logCategory) << "PARSE RECENTLY VIEWED";

    QObject* context = new QObject(this);
    QObject::connect(this, &NetflixFireTv::headersReady, context, [=](const QVariantMap& map) {
        //qCDebug(m_logCategory) << "HEADER RESPONSE RECEIVED...";
        if (recentModel->imageUrl().isEmpty()) {
            recentModel->imageUrl() = map.value("image").toString(); // doesn't work.
            recentModel->imageUrlChanged();
            qCDebug(m_logCategory) << "JSON show name: " << map.value("name").toString();
            //qCDebug(m_logCategory) << "JSON Image URL: " << map.value("image").toString();
            //qCDebug(m_logCategory) << "Model Image URL: " << recentModel->imageUrl();
        }

        if (map.value("type").toString() == "TVSeries") { // VideoObject is for an episdoe but this is a nested item of the show.
            QString type = "show";
        } else {
            QString type = "movie";
        }
        QString id = map.value("url").toString();
        id = id.right(id.length() - id.indexOf("title/"));
        QStringList commands = {"PLAY"};

        recentModel->addItem(id,map.value("name").toString(),map.value("description").toString(),"show",map.value("image").toString(),commands);

        // update the entity
        EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
        if (entity) {
            MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
            me->setBrowseModel(recentModel);
        }

        context->deleteLater();
        if (m_recentShows.count() > 1) {
            m_recentShows.removeLast();
            parseRecent(recentModel); // go again.

        } else {
            m_recentShows = QStringList(); // clear the list
        }
    });

    if (m_recentShows.count() > 0 && m_recentShows[0].contains("netflix://title/")) { // only iterate if there is something in the list.
        //qCDebug(m_logCategory) << "m_recentShows.last() " << m_recentShows.last();
        QString message = m_recentShows.last(); // items should be displayed in reverse order.
        int start = message.indexOf("netflix://title/");
        if (start != -1) {
            int end = message.indexOf(" flg");
            start += 10;
            QStringRef id = message.midRef(start, end-start);
            //qCDebug(m_logCategory) << "Start: " << start << ", End: " << end << ", id: " << id;
            if (id == "title/-1") {
                m_recentShows.removeLast(); // invalid entry, remove it
                parseRecent(recentModel); // call the loop again.
                return;
            } else {
                qCDebug(m_logCategory) << "Calling: " << "https://netflix.com/nl-en/" + id;
                QUrl url("https://www.netflix.com/nl-en/" + id);
                QNetworkRequest request;
                //request.setSslConfiguration(QSslConfiguration::defaultConfiguration());
                request.setUrl(url);

                QNetworkAccessManager * manager = new QNetworkAccessManager(this);
                QObject::connect(manager, SIGNAL(finished(QNetworkReply*)), this, SLOT(getDirect(QNetworkReply*)));
                manager->get(request);
            }
        }
    }

}


// START #### PARSE NETFLIX WEBPAGE FOR METADATA
QString NetflixFireTv::getHead(const QString& theWebPage) { // extracts the JSON portion of the Netflix header.
    //QRegExp filter("<script type(.+)" + QRegExp::escape("}</script>"));
    QRegExp filter(QRegExp::escape("<script type=\"application/ld+json\">{") + "(.+)" + QRegExp::escape("}</script>"));
    int result = filter.indexIn(theWebPage);
    if(result != -1) {
        QString output = "{" + filter.cap(1) + "}";
        return output.remove("@").simplified();
    } else { return QString(); }
}


void NetflixFireTv::getDirect(QNetworkReply * reply) {
    QUrl redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    //qCDebug(m_logCategory) << "Redirect url: " << reply->url().resolved(redirect);

    //qCDebug(m_logCategory) << "Raw header pairs: " << reply->rawHeaderPairs();
    //qCDebug(m_logCategory) << "Supports SSL: " << QSslSocket::supportsSsl();
    QString answer = reply->readAll();

    if (answer != "") {
        answer = getHead(answer);
        //qCDebug(m_logCategory).noquote() << "Full reply: " << answer;

        QMap<QString, QVariant> map;
        // convert to json
        QJsonParseError parseerror;
        QJsonDocument doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);
        if (parseerror.error != QJsonParseError::NoError) {
            qCWarning(m_logCategory) << "JSON error: " << parseerror.errorString();
            //qCWarning(m_logCategory) << "Location: " << parseerror.offset;
            return;
        }
        // create a map object
        map = doc.toVariant().toMap();
        emit headersReady(map);
    }
    reply->deleteLater();
}
// END #### PARSE NETFLIX WEBPAGE FOR METADATA

void NetflixFireTv::onPollingTimerTimeout() { getCurrentPlayer(); }

QString NetflixFireTv::convertSE(int series, int episode) { // convert seasons, episode to S00E00 format. Will remove once proper hierarchical browsing is supported.
    QString output = "S";
    if (series < 10) { output += "0" + QString::number(series);
    } else { output += QString::number(series); }

    if (episode < 10) { output += "E0" + QString::number(episode);
    } else { output += "E" + QString::number(episode); }
    output += ": ";

    return output;
}

QString NetflixFireTv::getCountryId(const QString& countryCode) {
    for (int i = 0; i < m_countryTable[0].length(); i++) {
        if (m_countryTable[0][i] == countryCode) { return m_countryTable[1][i]; }
    }
    qCWarning(m_logCategory) << "Error country code not found: " << countryCode;
    return "78"; // set to US if nothing is found.
}

bool NetflixFireTv::netflixActive() {
    QString result = sendAdbCommand("dumpsys window windows | grep mCurrentFocus");
    if (!result.contains("netflix")) { return true;
    } else { return false; }
}

bool NetflixFireTv::openNetflix() {
    // check if firetv is on, if not then turn it on and ensure Netflix is the active window.
    QString result = sendAdbCommand("dumpsys power | grep 'Display Power'"); // also can use "shell dumpsys power | grep mWakefulness" to see wake state - Awake/Asleep/etc.
    if (result.contains("state=OFF")) { sendAdbCommand("input key event 3"); } //press home to wake it up. May need to add power command - "input key event 26"?
    if (!netflixActive()) { sendAdbCommand("am start -n com.netflix.ninja/com.netflix.ninja.MainActivity"); }
    if (netflixActive()) { return true;
    } else { return false; }

}
