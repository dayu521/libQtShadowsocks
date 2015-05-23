/*
 * httpproxy.h - the source file of HttpProxy class
 *
 * Copyright (C) 2015 Symeon Huang <hzwhuang@gmail.com>
 *
 * This file is part of the libQtShadowsocks.
 *
 * libQtShadowsocks is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * libQtShadowsocks is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libQtShadowsocks; see the file LICENSE. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "httpproxy.h"
#include <QTcpSocket>
#include <QUrl>

using namespace QSS;

HttpProxy::HttpProxy(quint16 socks_port, const QHostAddress &http_addr, quint16 http_port, QObject *parent) : QTcpServer(parent)
{
    upstreamProxy = QNetworkProxy(QNetworkProxy::Socks5Proxy, "127.0.0.1", socks_port);
    this->setMaxPendingConnections(FD_SETSIZE);
    listenning = this->listen(http_addr, http_port);
}

HttpProxy::~HttpProxy()
{
    /*while(!socketList.isEmpty()) {
        socketList.takeLast()->deleteLater();
    }*/
}

void HttpProxy::incomingConnection(qintptr socketDescriptor)
{
    QTcpSocket *socket = new QTcpSocket(this);
    socketList.append(socket);
    connect(socket, &QTcpSocket::readyRead, this, &HttpProxy::onSocketReadyRead);
    connect(socket, &QTcpSocket::disconnected, this, &HttpProxy::onSocketDisconnected);
    socket->setSocketDescriptor(socketDescriptor);
}

void HttpProxy::onSocketReadyRead()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket *>(sender());

    QByteArray reqData = socket->readAll();
    int pos = reqData.indexOf("\r\n");
    QByteArray reqLine = reqData.left(pos);
    reqData.remove(0, pos + 2);

    QList<QByteArray> entries = reqLine.split(' ');
    QByteArray method = entries.value(0);
    QByteArray address = entries.value(1);
    QByteArray version = entries.value(2);

    QString host;
    quint16 port;

    if (method == "CONNECT") {
        /*
         * according to http://tools.ietf.org/html/draft-luotonen-ssl-tunneling-03
         * the first line would CONNECT HOST:PORT VERSION
         */
        QList<QByteArray> host_port_list = address.split(':');
        host = QString(host_port_list.first());
        port = host_port_list.last().toUShort();
    } else {
        QUrl url = QUrl::fromEncoded(address);
        if (!url.isValid()) {
            emit error("Invalid URL: " + url.toString());
            socket->disconnectFromHost();
            return;
        }
        host = url.host();
        port = url.port(80);
        QString req = url.path();
        if (url.hasQuery()) {
            req.append('?').append(url.query());
        }
        reqLine = method + " " + req.toUtf8() + " " + version + "\r\n";
        reqData.prepend(reqLine);
    }

    QString key = host + ':' + QString::number(port);
    QTcpSocket *proxySocket = socket->findChild<QTcpSocket *>(key);
    if (proxySocket) {
        proxySocket->write(reqData);
    } else {
        proxySocket = new QTcpSocket(socket);
        proxySocket->setObjectName(key);
        proxySocket->setProxy(upstreamProxy);
        if (method != "CONNECT") {
            proxySocket->setProperty("reqData", reqData);
            connect (proxySocket, &QTcpSocket::connected, this, &HttpProxy::onProxySocketConnected);
        } else {
            connect (proxySocket, &QTcpSocket::connected, this, &HttpProxy::onProxySocketConnectedHttps);
        }
        connect (proxySocket, &QTcpSocket::readyRead, this, &HttpProxy::onProxySocketReadyRead);
        connect (proxySocket, &QTcpSocket::disconnected, proxySocket, &QTcpSocket::deleteLater);
        proxySocket->connectToHost(host, port);
    }
}

void HttpProxy::onSocketReadyReadHttps()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket *>(sender());
    QString key = socket->property("httpsKey").toString();
    QTcpSocket *proxySocket = socket->findChild<QTcpSocket *>(key);
    if (proxySocket) {
        proxySocket->write(socket->readAll());
    } else {
        emit error("Can't find the proxy socket child to stream HTTPS connection");
    }
}

void HttpProxy::onSocketDisconnected()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    socketList.removeAll(socket);
    socket->deleteLater();
}

void HttpProxy::onProxySocketConnected()
{
    QTcpSocket *proxySocket = qobject_cast<QTcpSocket *>(sender());
    QByteArray reqData = proxySocket->property("reqData").toByteArray();
    proxySocket->write(reqData);
}

void HttpProxy::onProxySocketConnectedHttps()
{
    QTcpSocket *proxySocket = qobject_cast<QTcpSocket *>(sender());
    QTcpSocket *socket = qobject_cast<QTcpSocket *>(proxySocket->parent());
    disconnect(socket, &QTcpSocket::readyRead, this, &HttpProxy::onSocketReadyRead);
    connect(socket, &QTcpSocket::readyRead, this, &HttpProxy::onSocketReadyReadHttps);
    socket->setProperty("httpsKey", proxySocket->objectName());
    static const QByteArray httpsHeader = "HTTP/1.0 200 Connection established\r\n\r\n";
    socket->write(httpsHeader);
}

void HttpProxy::onProxySocketReadyRead()
{
    QTcpSocket *proxySocket = qobject_cast<QTcpSocket *>(sender());
    QTcpSocket *socket = qobject_cast<QTcpSocket *>(proxySocket->parent());
    socket->write(proxySocket->readAll());
}