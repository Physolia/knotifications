/*
    Copyright 2016 Kai Uwe Broulik <kde@privat.broulik.de>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QTimer>

#include <QDebug>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();

    // for simplicity we'll just send along progress-visible true whenever progress is set and otherwise false
    QCommandLineOption progressOption(QStringLiteral("progress"), QStringLiteral("Show progress, 0-100"), QStringLiteral("progress"));
    parser.addOption(progressOption);
    // same for count
    QCommandLineOption countOption(QStringLiteral("count"), QStringLiteral("Show count badge, number"), QStringLiteral("count"));
    parser.addOption(countOption);

    QCommandLineOption urgentOption(QStringLiteral("urgent"), QStringLiteral("Set urgent hint, flash task bar entry"));
    parser.addOption(urgentOption);

    parser.addPositionalArgument(QStringLiteral("desktop-filename"), QStringLiteral("Desktop file name for the application"));

    parser.process(app);

    if (parser.positionalArguments().count() != 1) {
        parser.showHelp(1); // never returns
    }

    QString launcherId = parser.positionalArguments().constFirst();
    if (!launcherId.startsWith(QLatin1String("application://"))) {
        launcherId.prepend(QLatin1String("application://"));
    }
    if (!launcherId.endsWith(QLatin1String(".desktop"))) {
        launcherId.append(QLatin1String(".desktop"));
    }

    QVariantMap properties;

    if (parser.isSet(progressOption)) {
        properties.insert(QStringLiteral("progress"), parser.value(progressOption).toInt() / 100.0);
        properties.insert(QStringLiteral("progress-visible"), true);
    } else {
        properties.insert(QStringLiteral("progress-visible"), false);
    }

    if (parser.isSet(countOption)) {
        properties.insert(QStringLiteral("count"), parser.value(countOption).toInt());
        properties.insert(QStringLiteral("count-visible"), true);
    } else {
        properties.insert(QStringLiteral("count-visible"), false);
    }

    properties.insert(QStringLiteral("urgent"), parser.isSet(urgentOption));

    QDBusMessage message = QDBusMessage::createSignal(QStringLiteral("/org/knotifications/UnityLauncherTest"),
                                                      QStringLiteral("com.canonical.Unity.LauncherEntry"),
                                                      QStringLiteral("Update"));
    message.setArguments({launcherId, properties});
    QDBusConnection::sessionBus().send(message);

    // FIXME can we detect that the message was sent to the bus?
    QTimer::singleShot(500, &app, QCoreApplication::quit);

    return app.exec();
}
