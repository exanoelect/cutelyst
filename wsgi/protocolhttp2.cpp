/*
 * Copyright (C) 2018 Daniel Nicoletti <dantti12@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include "protocolhttp2.h"

#include "socket.h"

#include <QLoggingCategory>

using namespace CWSGI;

Q_LOGGING_CATEGORY(CWSGI_H2, "cwsgi.http2")

#ifdef Q_CC_MSVC
#pragma pack(push)
#pragma pack(1)
#endif
struct h2_frame {
    quint8 size2;
    quint8 size1;
    quint8 size0;
    quint8 type;
    quint8 flags;
    quint8 rbit_stream_id3;
    quint8 rbit_stream_id2;
    quint8 rbit_stream_id1;
    quint8 rbit_stream_id0;
}
#ifdef Q_CC_MSVC
;
#pragma pack(pop)
#else
__attribute__ ((__packed__));
#endif

enum SettingsFlags {
    FlagSettingsAck = 0x1
};

enum PingFlags {
    FlagPingAck = 0x1
};

enum HeaderFlags {
    FlagHeadersEndStream = 0x1,
    FlagHeadersEndHeaders = 0x4,
    FlagHeadersPadded = 0x8,
    FlagHeadersPriority = 0x20,
};

enum DataFlags {
    FlagDataEndStream = 0x1,
    FlagDataPadded = 0x8,
};

enum FrameType {
    FrameData = 0x0,
    FrameHeaders = 0x1,
    FramePriority = 0x2,
    FrameRstStream = 0x3,
    FrameSettings = 0x4,
    FramePushPromise = 0x5,
    FramePing = 0x6,
    FrameGoaway = 0x7,
    FrameWindowUpdate = 0x8,
    FrameContinuation = 0x9
};

enum ErrorCodes {
    ErrorNoError = 0x0,
    ErrorProtocolError = 0x1,
    ErrorInternalError = 0x2,
    ErrorFlowControlError = 0x3,
    ErrorSettingsTimeout = 0x4,
    ErrorStreamClosed = 0x5,
    ErrorFrameSizeError = 0x6,
    ErrorRefusedStream = 0x7,
    ErrorCancel = 0x8,
    ErrorCompressionError = 0x9,
    ErrorConnectError = 0xA,
    ErrorEnhanceYourCalm  = 0xB,
    ErrorInadequateSecurity = 0xC,
    ErrorHttp11Required = 0xD
};

enum Settings {
    SETTINGS_HEADER_TABLE_SIZE = 0x1,
    SETTINGS_ENABLE_PUSH = 0x2,
    SETTINGS_MAX_CONCURRENT_STREAMS = 0x3,
    SETTINGS_INITIAL_WINDOW_SIZE = 0x4,
    SETTINGS_MAX_FRAME_SIZE = 0x5,
    SETTINGS_MAX_HEADER_LIST_SIZE = 0x6
};

quint32 h2_be32(const char *buf) {
    const quint32 *src = reinterpret_cast<const quint32 *>(buf);
    quint32 ret = 0;
    quint8 *ptr = reinterpret_cast<quint8 *>(&ret);
    ptr[0] = static_cast<quint8>((*src >> 24) & 0xff);
    ptr[1] = static_cast<quint8>((*src >> 16) & 0xff);
    ptr[2] = static_cast<quint8>((*src >> 8) & 0xff);
    ptr[3] = static_cast<quint8>(*src & 0xff);
    return ret;
}

quint32 h2_be24(const char *buf) {
    const quint32 *src = reinterpret_cast<const quint32 *>(buf);
    quint32 ret = 0;
    quint8 *ptr = reinterpret_cast<quint8 *>(&ret);
    ptr[0] = static_cast<quint8>((*src >> 16) & 0xff);
    ptr[1] = static_cast<quint8>((*src >> 8) & 0xff);
    ptr[2] = static_cast<quint8>(*src & 0xff);
    return ret;
}

quint16 h2_be16(const char *buf) {
    const quint32 *src = reinterpret_cast<const quint32 *>(buf);
    quint16 ret = 0;
    quint8 *ptr = reinterpret_cast<quint8 *>(&ret);
    ptr[0] = static_cast<quint8>((*src >> 8) & 0xff);
    ptr[1] = static_cast<quint8>(*src & 0xff);
    return ret;
}

#define PREFACE_SIZE 24

ProtocolHttp2::ProtocolHttp2(WSGI *wsgi) : Protocol(wsgi)
{
    if (m_bufferSize < 16384) {
        qFatal("HTTP/2 Protocol requires that buffer-size to be at least '16384' in size, current value is '%s'",
               QByteArray::number(m_bufferSize).constData());
    }
}

ProtocolHttp2::~ProtocolHttp2()
{

}

Protocol::Type ProtocolHttp2::type() const
{
    return Http2;
}

void ProtocolHttp2::readyRead(Socket *sock, QIODevice *io) const
{
    qint64 bytesAvailable = io->bytesAvailable();
    qCDebug(CWSGI_H2) << "readyRead available" << bytesAvailable;

    int len = io->read(sock->buffer + sock->buf_size, m_bufferSize - sock->buf_size);
    bytesAvailable -= len;

    if (len > 0) {
        sock->buf_size += len;
        int ret = 0;
        while (sock->buf_size && ret == 0) {
            qDebug() << QByteArray(sock->buffer, sock->buf_size);
            if (sock->connState == Socket::MethodLine) {
                if (sock->buf_size >= PREFACE_SIZE) {
                    if (memcmp(sock->buffer, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24) == 0) {
                        qCDebug(CWSGI_H2) << "Got preface" << sizeof(struct h2_frame);
                        memmove(sock->buffer, sock->buffer + PREFACE_SIZE, sock->buf_size - PREFACE_SIZE);
                        sock->buf_size -= PREFACE_SIZE;
                        sock->connState = Socket::H2Frames;

                        sendSettings(io, {
                                         { SETTINGS_MAX_FRAME_SIZE, qMin(m_bufferSize, qint64(2147483647)) },
                                     });
                    } else {
                        qCDebug(CWSGI_H2) << "Wrong preface";
                        ret = sendGoAway(io, ErrorProtocolError);
                    }
                }
            } else if (sock->connState == Socket::H2Frames) {
                if (sock->buf_size >= sizeof(struct h2_frame)) {
                    auto fr = reinterpret_cast<struct h2_frame *>(sock->buffer);
                    H2Frame frame;
                    frame.len = fr->size0 | (fr->size1 << 8) | (fr->size2 << 16);
                    frame.streamId = fr->rbit_stream_id0 |
                            (fr->rbit_stream_id1 << 8) |
                            (fr->rbit_stream_id2 << 16) |
                            ((fr->rbit_stream_id3 & ~0x80) << 24); // Ignore first bit
                    frame.type = fr->type;
                    frame.flags = fr->flags;
                    sock->pktsize = fr->size0 | (fr->size1 << 8) | (fr->size2 << 16);
                    sock->stream_id = fr->rbit_stream_id0 |
                            (fr->rbit_stream_id1 << 8) |
                            (fr->rbit_stream_id2 << 16) |
                            ((fr->rbit_stream_id3 & ~0x80) << 24); // Ignore first bit

                    qDebug() << "frame type" << fr->type << "flags" << fr->flags << "stream-id" << sock->stream_id;
                    qDebug() << "Reserved bit" << (fr->rbit_stream_id3) << (fr->rbit_stream_id3 & ~0x80) << (fr->rbit_stream_id3 & 0x80);
//                    qDebug() << "frame size" << quint32(fr->size1 | (fr->size2 << 8) | (fr->size3 << 16));
                    qDebug() << "frame size" << sock->pktsize << "available" << (sock->buf_size - sizeof(struct h2_frame));
                    if (sock->pktsize > (sock->buf_size - sizeof(struct h2_frame))) {
                        qDebug() << "need more data" << bytesAvailable;
                        return;
                    }

                    if (fr->type == FrameSettings) {
                        ret = parseSettings(sock, io, frame);

                        sock->buf_size -= 9 + sock->pktsize;
                        memmove(sock->buffer, sock->buffer + 9 + sock->pktsize, sock->buf_size);
                    } else if (fr->type == FramePriority) {
                        ret = parsePriority(sock, io, frame);

                        sock->buf_size -= 9 + sock->pktsize;
                        memmove(sock->buffer, sock->buffer + 9 + sock->pktsize, sock->buf_size);
                    } else if (fr->type == FrameHeaders) {
                        ret = parseHeaders(sock, io, frame);

                        sock->buf_size -= 9 + sock->pktsize;
                        memmove(sock->buffer, sock->buffer + 9 + sock->pktsize, sock->buf_size);
                    } else if (fr->type == FramePing) {
                        ret = parsePing(sock, io, frame);

                        sock->buf_size -= 9 + sock->pktsize;
                        memmove(sock->buffer, sock->buffer + 9 + sock->pktsize, sock->buf_size);
                    } else if (fr->type == FrameData) {
                        ret = parseData(sock, io, frame);

                        sock->buf_size -= 9 + sock->pktsize;
                        memmove(sock->buffer, sock->buffer + 9 + sock->pktsize, sock->buf_size);
                    } else if (fr->type == FramePushPromise) {
                        // PROTOCOL_ERROR
                        ret = sendGoAway(io, ErrorProtocolError);
                        return;
                    } else if (fr->type == FrameRstStream) {
                        ret = parseRstStream(sock, io, frame);

                        sock->buf_size -= 9 + sock->pktsize;
                        memmove(sock->buffer, sock->buffer + 9 + sock->pktsize, sock->buf_size);
                    } else if (fr->type == FrameGoaway) {
                        sock->connectionClose();
                        return;
                    } else {
                        qCDebug(CWSGI_H2) << "Unknown frame type" << fr->type;
                        // Implementations MUST ignore and discard any frame that has a type that is unknown.
                        sock->buf_size -= 9 + sock->pktsize;
                        memmove(sock->buffer, sock->buffer + 9 + sock->pktsize, sock->buf_size);
                    }
                }
            }
        }

        if (ret) {
            sock->connectionClose();
        }
    }
}

int ProtocolHttp2::parseSettings(Socket *sock, QIODevice *io, const H2Frame &fr) const
{
    qDebug() << "Consumming settings";
    if ((fr.flags & FlagSettingsAck && fr.len) || fr.len % 6) {
        sendGoAway(io, ErrorFrameSizeError);
        return 1;
    } else if (fr.streamId) {
        sendGoAway(io, ErrorProtocolError);
        return 1;
    }

    if (!(fr.flags & FlagSettingsAck)) {
        QVector<std::pair<quint16, quint32>> settings;
        uint pos = 0;
        while (sock->pktsize > pos) {
            quint16 identifier = h2_be16(sock->buffer + 9 + pos);
            quint32 value = h2_be32(sock->buffer + 9 + 2 + pos);
            settings.push_back({ identifier, value });
            //                            sock->pktsize -= 6;
            pos += 6;
            qDebug() << "SETTINGS" << identifier << value;
            if (identifier == SETTINGS_ENABLE_PUSH && value > 1) {
                return sendGoAway(io, ErrorProtocolError);
            } else if (identifier == SETTINGS_INITIAL_WINDOW_SIZE && value > 2147483647) {
                return sendGoAway(io, ErrorFlowControlError);
            } else if (identifier == SETTINGS_MAX_FRAME_SIZE && (value < 16384 || value > 16777215)) {
                return sendGoAway(io, ErrorProtocolError);
            }
        }
        sendSettingsAck(io);
    }

    return ErrorNoError;
}

int ProtocolHttp2::parseData(Socket *sock, QIODevice *io, const H2Frame &fr) const
{
    if (fr.streamId == 0) {
        return sendGoAway(io, ErrorProtocolError);
    }

    quint8 padLength = 0;
    if (fr.flags & FlagDataPadded) {
        padLength = quint8(*(sock->buffer + 9));
    } else {

    }
    qCDebug(CWSGI_H2) << "Frame data" << padLength;

    return ErrorNoError;
}

int ProtocolHttp2::parseHeaders(Socket *sock, QIODevice *io, const H2Frame &fr) const
{
    qDebug() << "Consumming headers";
    if (sock->stream_id == 0) {
        sendGoAway(io, ErrorProtocolError);
        return 1;
    }

    int pos = 0;
    char *ptr = sock->buffer + 9;
    quint8 padLength = 0;
    if (fr.flags & FlagHeadersPadded) {
        padLength = quint8(*(ptr + pos));
        pos += 1;
    }

    quint32 streamDependency = 0;
    quint8 weight = 0;
    if (fr.flags & FlagHeadersPriority) {
        streamDependency = h2_be32(ptr + pos);
        pos += 4;
        weight = quint8(*(ptr + pos)) + 1;
        pos += 1;
    }

    qDebug() << "Headers" << padLength << streamDependency << weight << QByteArray(ptr + pos, fr.len - pos);

    return 0;
}

int ProtocolHttp2::parsePriority(Socket *sock, QIODevice *io, const H2Frame &fr) const
{
    qDebug() << "Consumming priority";
    if (fr.len != 5) {
        sendGoAway(io, ErrorFrameSizeError);
        sock->connectionClose();
        return 1;
    } else if (fr.streamId == 0) {
        sendGoAway(io, ErrorProtocolError);
        sock->connectionClose();
        return 1;
    }

    uint pos = 0;
    while (fr.len > pos) {
        // TODO store/disable EXCLUSIVE bit
        quint32 exclusiveAndStreamDep = h2_be32(sock->buffer + 9 + pos);
        quint8 weigth = *reinterpret_cast<quint8 *>(sock->buffer + 9 + 4 + pos) + 1;
//                            settings.push_back({ identifier, value });
//                            sock->pktsize -= 6;

        if (fr.streamId == exclusiveAndStreamDep) {
            qDebug() << "PRIO error2" << exclusiveAndStreamDep << fr.streamId;
            sendGoAway(io, ErrorProtocolError);
            sock->connectionClose();
            return 1;
        }

        pos += 6;
        qDebug() << "PRIO" << exclusiveAndStreamDep << weigth;
    }

    return 0;
}

int ProtocolHttp2::parsePing(Socket *sock, QIODevice *io, const H2Frame &fr) const
{
    qCDebug(CWSGI_H2) << "Got ping" << fr.flags;
    if (fr.len != 8) {
        sendGoAway(io, ErrorFrameSizeError);
        sock->connectionClose();
        return 1;
    } else if (fr.streamId) {
        sendGoAway(io, ErrorProtocolError);
        sock->connectionClose();
        return 1;
    }

    if (!(fr.flags & FlagPingAck)) {
        sendPing(io, FlagPingAck, sock->buffer + 9, 8);
    }
    return 0;
}

int ProtocolHttp2::parseRstStream(Socket *sock, QIODevice *io, const H2Frame &fr) const
{
    if (sock->stream_id == 0) {
        sendGoAway(io, ErrorProtocolError);
        sock->connectionClose();
        return 1;
    } else if (sock->pktsize != 4) {
        sendGoAway(io, ErrorFrameSizeError);
        sock->connectionClose();
        return 1;
    }

    quint32 errorCode = h2_be32(sock->buffer + 9);
    qCDebug(CWSGI_H2) << "RST frame" << errorCode;

    return 0;
}

int ProtocolHttp2::sendGoAway(QIODevice *io, quint32 error) const
{
    quint64 data = error;
    sendFrame(io, FrameGoaway, 0, reinterpret_cast<const char *>(&data), 8);
    return error;
}

int ProtocolHttp2::sendSettings(QIODevice *io, const std::vector<std::pair<quint16, quint32>> &settings) const
{
    QByteArray data;
    for (const std::pair<quint16, quint32> &pair : settings) {
        data.append(quint8(pair.first >> 8));
        data.append(quint8(pair.first));
        data.append(quint8(pair.second >> 24));
        data.append(quint8(pair.second >> 16));
        data.append(quint8(pair.second >> 8));
        data.append(quint8(pair.second));
    }
    qDebug() << "Send settings" << data.toHex();
    return sendFrame(io, FrameSettings, 0, data.constData(), data.length());
}

int ProtocolHttp2::sendSettingsAck(QIODevice *io) const
{
    return sendFrame(io, FrameSettings, FlagSettingsAck);
}

int ProtocolHttp2::sendPing(QIODevice *io, quint8 flags, const char *data, qint32 dataLen) const
{
    return sendFrame(io, FramePing, flags, data, dataLen);
}

int ProtocolHttp2::sendFrame(QIODevice *io, quint8 type, quint8 flags, const char *data, qint32 dataLen) const
{
    h2_frame fr;

    fr.size2 = quint8(dataLen >> 16);
    fr.size1 = quint8(dataLen >> 8);
    fr.size0 = quint8(dataLen);
    fr.type = type;
    fr.flags = flags;
    fr.rbit_stream_id3 = 0;
    fr.rbit_stream_id2 = 0;
    fr.rbit_stream_id1 = 0;
    fr.rbit_stream_id0 = 0;

    qCDebug(CWSGI_H2) << "Sending frame"
                      << type
                      << flags
                      << quint32(fr.size0 | (fr.size1 << 8) | (fr.size2 << 16))
                      << quint32(fr.rbit_stream_id0 | (fr.rbit_stream_id1 << 8) | (fr.rbit_stream_id2 << 16) | (fr.rbit_stream_id3 << 24));

    qCDebug(CWSGI_H2) << "Frame" << QByteArray(reinterpret_cast<const char *>(&fr), sizeof(struct h2_frame));
    if (io->write(reinterpret_cast<const char *>(&fr), sizeof(struct h2_frame)) != sizeof(struct h2_frame)) {
        return -1;
    }
    if (dataLen && io->write(data, dataLen) != dataLen) {
        return -1;
    }
    return 0;
}

bool ProtocolHttp2::sendHeaders(QIODevice *io, Socket *sock, quint16 status, const QByteArray &dateHeader, const Cutelyst::Headers &headers)
{
    return false;
}
