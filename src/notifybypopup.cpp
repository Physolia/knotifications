/*
   Copyright (C) 2005-2009 by Olivier Goffart <ogoffart at kde.org>
   Copyright (C) 2008 by Dmitry Suzdalev <dimsuz@gmail.com>
   Copyright (C) 2014 by Martin Klapetek <mklapetek@kde.org>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) version 3, or any
   later version accepted by the membership of KDE e.V. (or its
   successor approved by the membership of KDE e.V.), which shall
   act as a proxy defined in Section 6 of version 3 of the license.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library.  If not, see <http://www.gnu.org/licenses/>.

 */

#include "notifybypopup.h"
#include "imageconverter.h"
#include "notifybypopupgrowl.h"

#include "kpassivepopup.h"
#include "knotifyconfig.h"
#include "knotification.h"

#include <QBuffer>
#include <QImage>
#include <QLabel>
#include <QTextDocument>
#include <QApplication>
#include <QDesktopWidget>
#include <QBoxLayout>
#include <QLayout>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusServiceWatcher>
#include <QDBusError>
#include <QDBusMessage>
#include <QXmlStreamReader>
#include <QDebug>
#include <QMap>
#include <QHash>
#include <QXmlStreamEntityResolver>

#include <kconfiggroup.h>
#include <KIconThemes/KIconLoader>
#include <KCodecs/KCharsets>

static const char dbusServiceName[] = "org.freedesktop.Notifications";
static const char dbusInterfaceName[] = "org.freedesktop.Notifications";
static const char dbusPath[] = "/org/freedesktop/Notifications";

class NotifyByPopupPrivate {
public:
    NotifyByPopupPrivate(NotifyByPopup *parent) : q(parent) {}
    /**
     * @internal
     * Fills the KPassivePopup with data
     */
    void fillPopup(KPassivePopup *popup, KNotification *notification, KNotifyConfig *config);
    /**
     * Make sure a popup is completely supported by the notification backend.
     * Changes the popup to be compatible if needed.
     * @param notification the notification data to check
     */
    void ensurePopupCompatibility(KNotification *notification);
    /**
     * Removes HTML from a given string. Replaces line breaks with \n and
     * HTML entities by their 'normal forms'.
     * @param string the HTML to remove.
     * @return the cleaned string.
     */
    QString stripHtml(const QString &text);
    /**
     * Sends notification to DBus "org.freedesktop.notifications" interface.
     * @param id knotify-sid identifier of notification
     * @param config notification data
     * @param update If true, will request the DBus service to update
                     the notification with new data from \c notification
     *               Otherwise will put new notification on screen
     * @return true for success or false if there was an error.
     */
    bool sendNotificationToGalagoServer(KNotification *notification, KNotifyConfig *config, bool update = false);
    /**
     * Sends request to close Notification with id to DBus "org.freedesktop.notifications" interface
     *  @param id knotify-side notification ID to close
     */
    void closeGalagoNotification(KNotification *notification);
    /**
     * Find the caption and the icon name of the application
     */
    void getAppCaptionAndIconName(KNotifyConfig *config, QString *appCaption, QString *iconName);
    /*
     * Query the dbus server for notification capabilities
     * If no DBus server is present, use fallback capabilities for KPassivePopup
     */
    void queryPopupServerCapabilities();

    // the y coordinate of the next position popup should appears
    int nextPosition;
    int animationTimer;
    /**
     * Specifies if DBus Notifications interface exists on session bus
     */
    bool dbusServiceExists;
    /**
     * DBus notification daemon capabilities cache.
     * Do not use this variable. Use #popupServerCapabilities() instead.
     * @see popupServerCapabilities
     */
    QStringList popupServerCapabilities;

    /**
     * In case we still don't know notification server capabilities,
     * we need to query those first. That's done in an async way
     * so we queue all notifications while waiting for the capabilities
     * to return, then process them from this queue
     */
    QList<QPair<KNotification*, KNotifyConfig*> > notificationQueue;
    /**
     * Whether the DBus notification daemon capability cache is up-to-date.
     */
    bool dbusServiceCapCacheDirty;

    /**
     * Keeps the map of notifications done in KPassivePopup
     */
    QMap<KNotification*, KPassivePopup *> passivePopups;

    /*
     * As we communicate with the notification server over dbus
     * we use only ids, this is for fast KNotifications lookup
     */
    QHash<uint, KNotification*> galagoNotifications;

    NotifyByPopup * const q;

    /**
     * A class for resolving HTML entities in XML documents (used
     * during HTML stripping)
     */
    class HtmlEntityResolver : public QXmlStreamEntityResolver
    {
        QString resolveUndeclaredEntity(const QString &name);
    };

};

//---------------------------------------------------------------------------------------

NotifyByPopup::NotifyByPopup(QObject *parent) 
  : KNotifyPlugin(parent),
    d(new NotifyByPopupPrivate(this))
{
    d->animationTimer = 0;
    d->dbusServiceExists = false;
    d->dbusServiceCapCacheDirty = true;

    QRect screen = QApplication::desktop()->availableGeometry();
    d->nextPosition = screen.top();

    // check if service already exists on plugin instantiation
    QDBusConnectionInterface *interface = QDBusConnection::sessionBus().interface();
    d->dbusServiceExists = interface && interface->isServiceRegistered(dbusServiceName);

    if (d->dbusServiceExists) {
        onServiceOwnerChanged(dbusServiceName, QString(), "_"); //connect signals
    }

    // to catch register/unregister events from service in runtime
    QDBusServiceWatcher *watcher = new QDBusServiceWatcher(this);
    watcher->setConnection(QDBusConnection::sessionBus());
    watcher->setWatchMode(QDBusServiceWatcher::WatchForOwnerChange);
    watcher->addWatchedService(dbusServiceName);
    connect(watcher, SIGNAL(serviceOwnerChanged(QString,QString,QString)),
            SLOT(onServiceOwnerChanged(QString,QString,QString)));

    if (!d->dbusServiceExists) {
        bool startfdo = false;
#ifdef Q_WS_WIN
        startfdo = true;
#else
        if (qgetenv("KDE_FULL_SESSION").isEmpty()) {
            QDBusMessage message = QDBusMessage::createMethodCall("org.freedesktop.DBus",
                                                                  "/org/freedesktop/DBus",
                                                                  "org.freedesktop.DBus",
                                                                  "ListActivatableNames");

            // FIXME - this should be async
            QDBusReply<QStringList> reply = QDBusConnection::sessionBus().call(message);
            if (reply.isValid() && reply.value().contains(dbusServiceName)) {
                startfdo = true;
                // We need to set d->dbusServiceExists to true because dbus might be too slow
                // starting the service and the first call to NotifyByPopup::notify
                // might not have had the service up, by setting this to true we
                // guarantee it will still go through dbus and dbus will do the correct
                // thing and wait for the service to go up
                d->dbusServiceExists = true;
            }
        }
#endif
        if (startfdo) {
            QDBusConnection::sessionBus().interface()->startService(dbusServiceName);
        }
    }
}


NotifyByPopup::~NotifyByPopup()
{
    Q_FOREACH (KPassivePopup *p, d->passivePopups) {
        p->deleteLater();
    }

    delete d;
}

void NotifyByPopup::notify(KNotification *notification, KNotifyConfig *notifyConfig)
{
    if (d->passivePopups.contains(notification) || d->galagoNotifications.contains(notification->id())) {
        // notification is alrady on the screen, do nothing
        finish(notification);
        return;
    }

    // check if Notifications DBus service exists on bus, use it if it does
    if (d->dbusServiceExists) {
        if (d->dbusServiceCapCacheDirty) {
            // if we don't have the server capabilities yet, we need to query for them first;
            // as that is an async dbus operation, we enqueue the notification and process them
            // when we receive dbus reply with the server capabilities
            d->notificationQueue.append(qMakePair(notification, notifyConfig));
            d->queryPopupServerCapabilities();
        } else {
            if (!d->sendNotificationToGalagoServer(notification, notifyConfig)) {
                finish(notification); //an error ocurred.
            }
        }
        return;
    }

    // Persistent     => 0  == infinite timeout
    // CloseOnTimeout => -1 == let the server decide
    int timeout = notification->flags() & KNotification::Persistent ? 0 : -1;

    // if Growl can display our popups, use that instead
    if (NotifyByPopupGrowl::canPopup()) {
        d->ensurePopupCompatibility(notification);

        QString appCaption, iconName;
        d->getAppCaptionAndIconName(notifyConfig, &appCaption, &iconName);

        KIconLoader iconLoader(iconName);
        QPixmap appIcon = iconLoader.loadIcon(iconName, KIconLoader::Small);

        NotifyByPopupGrowl::popup(&appIcon, timeout, appCaption, notification->text());

        // Finish immediately, because current NotifyByPopupGrowl can't callback
        finish(notification);
        delete notifyConfig;
        return;
    }

    // last fallback - display the popup using KPassivePopup
    KPassivePopup *pop = new KPassivePopup(notification->widget());
    d->passivePopups.insert(notification, pop);
    d->fillPopup(pop, notification, notifyConfig);

    QRect screen = QApplication::desktop()->availableGeometry();
    pop->setAutoDelete(true);
    connect(pop, SIGNAL(destroyed()), this, SLOT(onPassivePopupDestroyed()));

    pop->setTimeout(timeout);
    pop->adjustSize();
    pop->show(QPoint(screen.left() + screen.width()/2 - pop->width()/2 , d->nextPosition));
    d->nextPosition += pop->height();
}

void NotifyByPopup::onPassivePopupDestroyed()
{
    const QObject *destroyedPopup = sender();

    if (!destroyedPopup) {
        return;
    }

    QMap<KNotification*, KPassivePopup*>::iterator it;
    for (it = d->passivePopups.begin(); it != d->passivePopups.end(); ++it) {
        QObject *popup = it.value();
        if (popup && popup == destroyedPopup) {
            finish(it.key());
            d->passivePopups.remove(it.key());
            break;
        }
    }

    //relocate popup
    if (!d->animationTimer) {
        d->animationTimer = startTimer(10);
    }
}

void NotifyByPopup::timerEvent(QTimerEvent *event)
{
    if (event->timerId() != d->animationTimer) {
        return KNotifyPlugin::timerEvent(event);
    }

    bool cont = false;
    QRect screen = QApplication::desktop()->availableGeometry();
    d->nextPosition = screen.top();

    Q_FOREACH (KPassivePopup *popup, d->passivePopups)
    {
        int y = popup->pos().y();
        if (y > d->nextPosition) {
            y = qMax(y - 5, d->nextPosition);
            d->nextPosition = y + popup->height();
            cont = cont || y != d->nextPosition;
            popup->move(popup->pos().x(), y);
        } else {
            d->nextPosition += popup->height();
        }
    }

    if (!cont) {
        killTimer(d->animationTimer);
        d->animationTimer = 0;
    }
}

void NotifyByPopup::onPassivePopupLinkClicked(const QString &link)
{
    unsigned int id = link.section('/' , 0 , 0).toUInt();
    unsigned int action = link.section('/' , 1 , 1).toUInt();

    if (id == 0 || action == 0) {
        return;
    }

    emit actionInvoked(id, action);
}

void NotifyByPopup::close(KNotification *notification)
{
    if (d->dbusServiceExists) {
        d->closeGalagoNotification(notification);
    }

    if (d->passivePopups.contains(notification)) {
        // this will call onPassivePopupDestroyed()
        // which will call finish() on the notification
        d->passivePopups[notification]->deleteLater();
    }
}

void NotifyByPopup::update(KNotification *notification, KNotifyConfig *notifyConfig)
{
    if (d->passivePopups.contains(notification)) {
        KPassivePopup *p = d->passivePopups[notification];
        d->fillPopup(p, notification, notifyConfig);
        return;
    }

    // if Notifications DBus service exists on bus,
    // it'll be used instead
    if (d->dbusServiceExists) {
        d->sendNotificationToGalagoServer(notification, notifyConfig, true);
        return;
    }

    // otherwise, just display a new Growl notification
    if (NotifyByPopupGrowl::canPopup()) {
        notify(notification, notifyConfig);
    }
}

void NotifyByPopup::onServiceOwnerChanged(const QString &serviceName, const QString &oldOwner, const QString &newOwner)
{
    // close all notifications we currently hold reference to
    Q_FOREACH (KNotification *n, d->galagoNotifications.values()) {
        finished(n);
    }
    Q_FOREACH (KNotification *n, d->passivePopups.keys()) {
        finished(n);
    }
    d->galagoNotifications.clear();
    d->passivePopups.clear();

    d->dbusServiceCapCacheDirty = true;
    d->popupServerCapabilities.clear();
    d->notificationQueue.clear();

    if (newOwner.isEmpty()) {
        d->dbusServiceExists = false;
    } else if (oldOwner.isEmpty()) {
        d->dbusServiceExists = true;

        // connect to action invocation signals
        bool connected = QDBusConnection::sessionBus().connect(QString(), // from any service
                                                               dbusPath,
                                                               dbusInterfaceName,
                                                               "ActionInvoked",
                                                               this,
                                                               SLOT(onGalagoNotificationActionInvoked(uint,QString)));
        if (!connected) {
            qWarning() << "warning: failed to connect to ActionInvoked dbus signal";
        }

        connected = QDBusConnection::sessionBus().connect(QString(), // from any service
                                                          dbusPath,
                                                          dbusInterfaceName,
                                                          "NotificationClosed",
                                                          this,
                                                          SLOT(onGalagoNotificationClosed(uint,uint)));
        if (!connected) {
            qWarning() << "warning: failed to connect to NotificationClosed dbus signal";
        }
    }
}

void NotifyByPopup::onGalagoNotificationActionInvoked(uint notificationId, const QString &actionKey)
{
    KNotification *n = d->galagoNotifications[notificationId];
    if (n == 0) {
        qWarning() << "Failed to find KNotification id for dbus_id" << notificationId << "- action not triggered";
        return;
    }

    emit actionInvoked(n->id(), actionKey.toUInt());
    // now close notification - similar to popup behaviour
    // (popups are hidden after link activation - see 'connects' of linkActivated signal above)
    d->closeGalagoNotification(n);
}

void NotifyByPopup::onGalagoNotificationClosed(uint dbus_id, uint reason)
{
    Q_UNUSED(reason)
    KNotification *n = d->galagoNotifications[dbus_id];
    if (n == 0) {
        qWarning() << "Failed to find KNotification for dbus_id" << dbus_id;
        return;
    }
    d->galagoNotifications.remove(dbus_id);
    finished(n);
    // The popup bubble is the only user facing part of a notification,
    // if the user closes the popup, it means he wants to get rid
    // of the notification completely, including playing sound etc
    // Therefore we close the KNotification completely after closing
    // the popup
    n->close();
}

void NotifyByPopup::onGalagoServerReply(QDBusPendingCallWatcher *watcher)
{
    KNotification *notification = watcher->property("notificationObject").value<KNotification*>();
    if (!notification) {
        qWarning() << "Invalid notification object passed in DBus reply watcher; notification will probably break";
        return;
    }

    QDBusPendingReply<uint> reply = *watcher;

    d->galagoNotifications.insert(reply.argumentAt<0>(), notification);
    watcher->deleteLater();
}

void NotifyByPopup::onGalagoServerCapabilitiesReceived(const QStringList &capabilities)
{
    d->popupServerCapabilities = capabilities;
    d->dbusServiceCapCacheDirty = false;

    // re-run notify() on all enqueued notifications
    for (int i = 0; i < d->notificationQueue.size(); i++) {
        notify(d->notificationQueue.at(i).first, d->notificationQueue.at(i).second);
    }

    d->notificationQueue.clear();
}

void NotifyByPopupPrivate::getAppCaptionAndIconName(KNotifyConfig *notifyConfig, QString *appCaption, QString *iconName)
{
    KConfigGroup globalgroup(&(*notifyConfig->eventsfile), QString("Global"));
    *appCaption = globalgroup.readEntry("Name", globalgroup.readEntry("Comment", notifyConfig->appname));

    KConfigGroup eventGroup(&(*notifyConfig->eventsfile), QString("Event/%1").arg(notifyConfig->eventid));
    if (eventGroup.hasKey("IconName")) {
        *iconName = eventGroup.readEntry("IconName", notifyConfig->appname);
    } else {
        *iconName = globalgroup.readEntry("IconName", notifyConfig->appname);
    }
}

void NotifyByPopupPrivate::fillPopup(KPassivePopup *popup, KNotification *notification, KNotifyConfig *notifyConfig)
{
    QString appCaption;
    QString iconName;
    getAppCaptionAndIconName(notifyConfig, &appCaption, &iconName);

    // If we're at this place, it means there's no D-Bus service for notifications
    // so we don't need to do D-Bus query for the capabilities.
    // If queryPopupServerCapabilities() finds no service, it sets the KPassivePopup
    // capabilities immediately, so we don't need to wait for callback as in the case
    // of galago notifications
    queryPopupServerCapabilities();
    ensurePopupCompatibility(notification);

    KIconLoader iconLoader(iconName);
    QPixmap appIcon = iconLoader.loadIcon(iconName, KIconLoader::Small);

    QWidget *vb = popup->standardView(notification->title().isEmpty() ? appCaption : notification->title(),
                                      notification->pixmap().isNull() ? notification->text() : QString(),
                                      appIcon);

    if (!notification->pixmap().isNull()) {
        const QPixmap pix = notification->pixmap();
        QHBoxLayout *hbox = new QHBoxLayout(vb);

        QLabel *pil = new QLabel();
        pil->setPixmap(pix);
        pil->setScaledContents(true);

        if (pix.height() > 80 && pix.height() > pix.width()) {
            pil->setMaximumHeight(80);
            pil->setMaximumWidth(80 * pix.width() / pix.height());
        } else if(pix.width() > 80 && pix.height() <= pix.width()) {
            pil->setMaximumWidth(80);
            pil->setMaximumHeight(80*pix.height()/pix.width());
        }

        hbox->addWidget(pil);

        QVBoxLayout *vb2 = new QVBoxLayout(vb);
        QLabel *msg = new QLabel(notification->text());
        msg->setAlignment(Qt::AlignLeft);

        vb2->addWidget(msg);

        hbox->addLayout(vb2);

        vb->layout()->addItem(hbox);
    }


    if (!notification->actions().isEmpty()) {
        QString linkCode = QString::fromLatin1("<p align=\"right\">");
        int i = 0;
        Q_FOREACH (const QString &it, notification->actions()) {
            i++;
            linkCode += QString::fromLatin1("&nbsp;<a href=\"%1/%2\">%3</a>").arg(QString::number(notification->id()), QString::number(i), it.toHtmlEscaped());
        }

        linkCode += QString::fromLatin1("</p>");
        QLabel *link = new QLabel(linkCode , vb );
        link->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
        link->setOpenExternalLinks(false);
        //link->setAlignment( AlignRight );
        QObject::connect(link, SIGNAL(linkActivated(const QString &)), q, SLOT(onPassivePopupLinkClicked(const QString& ) ) );
        QObject::connect(link, SIGNAL(linkActivated(const QString &)), popup, SLOT(hide()));
    }

    popup->setView( vb );
}

bool NotifyByPopupPrivate::sendNotificationToGalagoServer(KNotification *notification, KNotifyConfig *notifyConfig_nocheck, bool update)
{
    uint updateId = galagoNotifications.key(notification, 0);

    if (update) {
        if (updateId == 0) {
            // we have nothing to update; the notification we're trying to update
            // has been already closed
            return false;
        }
    }

    QDBusMessage dbusNotificationMessage = QDBusMessage::createMethodCall(dbusServiceName, dbusPath, dbusInterfaceName, "Notify");

    QList<QVariant> args;

    QString appCaption;
    QString iconName;
    getAppCaptionAndIconName(notifyConfig_nocheck, &appCaption, &iconName);

    // FIXME: rename this to something better reflecting what this is doing...maybe
    ensurePopupCompatibility(notification);

    args.append(appCaption); // app_name
    args.append(updateId);  // notification to update
    args.append(iconName); // app_icon
    args.append(notification->title().isEmpty() ? appCaption : notification->title()); // summary
    args.append(notification->text()); // body
    // galago spec defines action list to be list like
    // (act_id1, action1, act_id2, action2, ...)
    //
    // assign id's to actions like it's done in fillPopup() method
    // (i.e. starting from 1)
    QStringList actionList;
    int actId = 0;
    Q_FOREACH (const QString &actionName, notification->actions()) {
        actId++;
        actionList.append(QString::number(actId));
        actionList.append(actionName);
    }

    args.append(actionList); // actions

    QVariantMap hintsMap;
    // Add the application name to the hints.
    // According to fdo spec, the app_name is supposed to be the applicaton's "pretty name"
    // but in some places it's handy to know the application name itself
    if (!notification->appName().isEmpty()) {
        hintsMap["x-kde-appname"] = notification->appName();
    }

    //FIXME - reenable/fix
    // let's see if we've got an image, and store the image in the hints map
    if (!notification->pixmap().isNull()) {
        QByteArray pixmapData;
        QBuffer buffer(&pixmapData);
        buffer.open(QIODevice::WriteOnly);
        notification->pixmap().save(&buffer, "PNG");
        buffer.close();
        hintsMap["image_data"] = ImageConverter::variantForImage(QImage::fromData(pixmapData));
    }

    args.append(hintsMap); // hints

    // Persistent     => 0  == infinite timeout
    // CloseOnTimeout => -1 == let the server decide
    int timeout = notification->flags() & KNotification::Persistent ? 0 : -1;

    args.append(timeout); // expire timout

    dbusNotificationMessage.setArguments(args);

    QDBusPendingCall notificationCall = QDBusConnection::sessionBus().asyncCall(dbusNotificationMessage, -1);

    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(notificationCall, q);
    watcher->setProperty("notificationObject", QVariant::fromValue<KNotification*>(notification));

    QObject::connect(watcher, SIGNAL(finished(QDBusPendingCallWatcher*)),
                     q, SLOT(onGalagoServerReply(QDBusPendingCallWatcher*)));

    return true;
}

void NotifyByPopupPrivate::closeGalagoNotification(KNotification *notification)
{
    uint galagoId = galagoNotifications.key(notification, 0);

    if (galagoId == 0) {
        qDebug() << "not found dbus id to close" << notification->id();
        return;
    }

    QDBusMessage m = QDBusMessage::createMethodCall(dbusServiceName, dbusPath,
                                                    dbusInterfaceName, "CloseNotification");
    QList<QVariant> args;
    args.append(galagoId);
    m.setArguments(args);

    // send(..) does not block
    bool queued = QDBusConnection::sessionBus().send(m);

    if (!queued) {
        qWarning() << "Failed to queue dbus message for closing a notification";
    }

}

void NotifyByPopupPrivate::queryPopupServerCapabilities()
{
    if (!dbusServiceExists) {
        if (NotifyByPopupGrowl::canPopup()) {
            popupServerCapabilities = NotifyByPopupGrowl::capabilities();
        } else {
            // Return capabilities of the KPassivePopup implementation
            popupServerCapabilities = QStringList() << "actions" << "body" << "body-hyperlinks"
                                                      << "body-markup" << "icon-static";
        }
    }

    if (dbusServiceCapCacheDirty) {
        QDBusMessage m = QDBusMessage::createMethodCall(dbusServiceName,
                                                        dbusPath,
                                                        dbusInterfaceName,
                                                        "GetCapabilities");

        QDBusConnection::sessionBus().callWithCallback(m,
                                                       q,
                                                       SLOT(onGalagoServerCapabilitiesReceived(QStringList)),
                                                       0,
                                                       -1);
    }
}

void NotifyByPopupPrivate::ensurePopupCompatibility(KNotification *notification)
{
    if (!popupServerCapabilities.contains("actions")) {
        notification->setActions(QStringList());
    }

    if (!popupServerCapabilities.contains("body-markup")) {
        if (notification->title().startsWith("<html>")) {
            notification->setTitle(stripHtml(notification->title()));
        }
        if (notification->text().startsWith("<html>")) {
            notification->setText(stripHtml(notification->text()));
        }
    }
}

QString NotifyByPopupPrivate::stripHtml(const QString &text)
{
    QXmlStreamReader r("<elem>" + text + "</elem>");
    HtmlEntityResolver resolver;
    r.setEntityResolver(&resolver);
    QString result;
    while (!r.atEnd()) {
        r.readNext();
        if (r.tokenType() == QXmlStreamReader::Characters) {
            result.append(r.text());
        } else if (r.tokenType() == QXmlStreamReader::StartElement && r.name() == "br") {
            result.append("\n");
        }
    }

    if (r.hasError()) {
        // XML error in the given text, just return the original string
        qWarning() << "Notification to send to backend which does "
                         "not support HTML, contains invalid XML:"
                      << r.errorString() << "line" << r.lineNumber()
                      << "col" << r.columnNumber();
        return text;
    }

    return result;
}

QString NotifyByPopupPrivate::HtmlEntityResolver::resolveUndeclaredEntity(const QString &name)
{
    QString result = QXmlStreamEntityResolver::resolveUndeclaredEntity(name);

    if (!result.isEmpty()) {
        return result;
    }

    QChar ent = KCharsets::fromEntity('&' + name);

    if (ent.isNull()) {
        qWarning() << "Notification to send to backend which does "
                      "not support HTML, contains invalid entity: "
                   << name;
        ent = ' ';
    }

    return QString(ent);
}

#include "notifybypopup.moc"
