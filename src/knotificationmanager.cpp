/* This file is part of the KDE libraries
   Copyright (C) 2005 Olivier Goffart <ogoffart at kde.org>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License version 2 as published by the Free Software Foundation.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/

#include "knotificationmanager_p.h"
#include "knotification.h"

#include <QDebug>
#include <QHash>
#include <QWidget>
#include <QDBusConnection>
#include <QPointer>

#include "knotify_interface.h"

#include "knotifyconfig.h"

typedef QHash<QString, QString> Dict;

struct KNotificationManager::Private {
    QHash<int, KNotification *> notifications;
    org::kde::KNotify *knotify;
};

class KNotificationManagerSingleton
{
public:
    KNotificationManager instance;
};

Q_GLOBAL_STATIC(KNotificationManagerSingleton, s_self)

KNotificationManager *KNotificationManager::self()
{
    return &s_self()->instance;
}

KNotificationManager::KNotificationManager()
    : d(new Private)
{
    QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface();
    if (!bus->isServiceRegistered("org.kde.knotify")) {
        QDBusReply<void> reply = bus->startService("org.kde.knotify");
        if (!reply.isValid()) {
            qCritical() << "Couldn't start knotify from org.kde.knotify.service:" << reply.error();
        }
    }
    d->knotify =
        new org::kde::KNotify(QLatin1String("org.kde.knotify"), QLatin1String("/Notify"), QDBusConnection::sessionBus(), this);
    connect(d->knotify, SIGNAL(notificationClosed(int)),
            this, SLOT(notificationClosed(int)));
    connect(d->knotify, SIGNAL(notificationActivated(int,int)),
            this, SLOT(notificationActivated(int,int)));
}

KNotificationManager::~KNotificationManager()
{
    delete d->knotify;
    delete d;
}

void KNotificationManager::notificationActivated(int id, int action)
{
    if (d->notifications.contains(id)) {
        qDebug() << id << " " << action;
        KNotification *n = d->notifications[id];
        d->notifications.remove(id);
        n->activate(action);
    }
}

void KNotificationManager::notificationClosed(KNotification *notification)
{
    if (d->notifications.contains(notification->id())) {
        qDebug() << notification->id();
        KNotification *n = d->notifications[notification->id()];
        d->notifications.remove(notification->id());
        n->close();
    }
}

void KNotificationManager::close(int id, bool force)
{
    if (force || d->notifications.contains(id)) {
        d->notifications.remove(id);
        qDebug() << id;
        d->knotify->closeNotification(id);
    }
}

int KNotificationManager::notify(KNotification *n)
{
    //FIXME: leaking?
    KNotifyConfig *notifyConfig = new KNotifyConfig(n->appName(), n->contexts(), n->eventId());

    QString notifyActions = notifyConfig->readEntry("Action");
    qDebug() << "Got notification \"" << n->eventId() <<"\" with actions:" << notifyActions;

    d->notifications.insert(++d->notifyIdCounter, n);

    Q_FOREACH (const QString &action, notifyActions.split('|')) {
        if (!d->notifyPlugins.contains(action)) {
            qDebug() << "No plugin for action" << action;
            continue;
        }

        KNotifyPlugin *notifyPlugin = d->notifyPlugins[action];
        n->ref();
        qDebug() << "calling notify on" << notifyPlugin->optionName();
        notifyPlugin->notify(n, notifyConfig);
    }

void KNotificationManager::insert(KNotification *n, int id)
{
    d->notifications.insert(id, n);
    return d->notifyIdCounter;
}

void KNotificationManager::update(KNotification *n)
{
    QByteArray pixmapData;
    if (!n->pixmap().isNull()) {
        QBuffer buffer(&pixmapData);
        buffer.open(QIODevice::WriteOnly);
        n->pixmap().save(&buffer, "PNG");
    }

    d->knotify->update(id, n->title(), n->text(), pixmapData, n->actions());
}

void KNotificationManager::reemit(KNotification *n)
{
    QVariantList contextList;
    typedef QPair<QString, QString> Context;
    foreach (const Context &ctx, n->contexts()) {
//      qDebug() << "add context " << ctx.first << "-" << ctx.second;
        QVariantList vl;
        vl << ctx.first << ctx.second;
        contextList << vl;
    }

    d->knotify->reemit(id, contextList);
}

#include "moc_knotificationmanager_p.cpp"
