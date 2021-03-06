/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/base/system_error.h"
#include "mongo/db/stats/counters.h"
#include "mongo/transport/asio_utils.h"
#include "mongo/transport/ticket_asio.h"
#include "mongo/util/log.h"

#include "mongo/transport/session_asio.h"

namespace mongo {
namespace transport {
namespace {
	//mongo协议头部长度  TransportLayerASIO::ASIOSourceTicket::fillImpl
constexpr auto kHeaderSize = sizeof(MSGHEADER::Value);

}  // namespace

std::shared_ptr<TransportLayerASIO::ASIOSession> TransportLayerASIO::ASIOTicket::getSession() {
    auto session = _session.lock();
    if (!session || !session->isOpen()) {
        finishFill(Ticket::SessionClosedStatus);
        return nullptr;
    }
    return session;
}

//ServiceStateMachine::_sourceMessage可以看出，kSynchronous模式为true，adaptive模式为false
bool TransportLayerASIO::ASIOTicket::isSync() const {
    return _fillSync;
}

TransportLayerASIO::ASIOTicket::ASIOTicket(const ASIOSessionHandle& session, Date_t expiration)
    : _session(session), _sessionId(session->id()), _expiration(expiration) {}

TransportLayerASIO::ASIOSourceTicket::ASIOSourceTicket(const ASIOSessionHandle& session,
                                                       Date_t expiration,
                                                       Message* msg)
    : ASIOTicket(session, expiration), _target(msg) {}

TransportLayerASIO::ASIOSinkTicket::ASIOSinkTicket(const ASIOSessionHandle& session,
                                                   Date_t expiration,
                                                   const Message& msg)
    : ASIOTicket(session, expiration), _msgToSend(msg) {}
//TransportLayerASIO::ASIOSourceTicket::_headerCallback
//_headerCallback对header读取后解析header头部获取到对应的msg长度，然后开始body部分处理
void TransportLayerASIO::ASIOSourceTicket::_bodyCallback(const std::error_code& ec, size_t size) {
    if (ec) {
        finishFill(errorCodeToStatus(ec));
        return;
    }

    _target->setData(std::move(_buffer));
    networkCounter.hitPhysicalIn(_target->size());
	//TransportLayerASIO::ASIOTicket::finishFill  
    finishFill(Status::OK()); //包体内容读完后，开始下一阶段的处理  
    //报文读取完后的下一阶段就是报文内容处理，开始走ServiceStateMachine::_processMessage
}

//TransportLayerASIO::ASIOSourceTicket::fillImpl
//fillImpl从协议栈读取到数据后开始解析，如果数据内容足够，则调用finishFill做进一步处理，否则继续调用read继续读数据
void TransportLayerASIO::ASIOSourceTicket::_headerCallback(const std::error_code& ec, size_t size) {
    if (ec) {
        finishFill(errorCodeToStatus(ec));
        return;
    }

    auto session = getSession();
    if (!session)
        return;

    MSGHEADER::View headerView(_buffer.get());
    auto msgLen = static_cast<size_t>(headerView.getMessageLength());
    if (msgLen < kHeaderSize || msgLen > MaxMessageSizeBytes) {
        StringBuilder sb;
        sb << "recv(): message msgLen " << msgLen << " is invalid. "
           << "Min " << kHeaderSize << " Max: " << MaxMessageSizeBytes;
        const auto str = sb.str();
        LOG(0) << str;
        finishFill(Status(ErrorCodes::ProtocolError, str));
        return;
    }

	//说明数据部分也读取出来了，一个完整的mongo报文读取完毕
    if (msgLen == size) {
        finishFill(Status::OK());
        return;
    }

	//内容还不够一个mongo协议报文，继续读取body长度字节的数据，读取完毕后开始body处理
    _buffer.realloc(msgLen); //注意这里是realloc，保证头部和body在同一个buffer中
    MsgData::View msgView(_buffer.get());


	//读取数据 TransportLayerASIO::ASIOSession::read
    session->read(isSync(),
                  asio::buffer(msgView.data(), msgView.dataLen()),
                  [this](const std::error_code& ec, size_t size) { _bodyCallback(ec, size); });
}

//读取mongo协议报文头部，协议栈返回后调用_headerCallback回调
//TransportLayerASIO::ASIOTicket::fill
void TransportLayerASIO::ASIOSourceTicket::fillImpl() {
    auto session = getSession();
    if (!session)
        return;

    const auto initBufSize = kHeaderSize;
    _buffer = SharedBuffer::allocate(initBufSize);

	//读取数据 TransportLayerASIO::ASIOSession::read
    session->read(isSync(),
                  asio::buffer(_buffer.get(), initBufSize), //先读取头部字段出来
                  [this](const std::error_code& ec, size_t size) { _headerCallback(ec, size); });
}

void TransportLayerASIO::ASIOSinkTicket::_sinkCallback(const std::error_code& ec, size_t size) {
    networkCounter.hitPhysicalOut(_msgToSend.size()); //发送的网络字节数统计
    finishFill(ec ? errorCodeToStatus(ec) : Status::OK());
}

void TransportLayerASIO::ASIOSinkTicket::fillImpl() {
    auto session = getSession();
    if (!session)
        return;

    session->write(isSync(),
                   asio::buffer(_msgToSend.buf(), _msgToSend.size()),
                   [this](const std::error_code& ec, size_t size) { _sinkCallback(ec, size); });
}

//ServiceStateMachine::_sourceMessage->TransportLayerASIO::asyncWait->TransportLayerASIO::ASIOTicket::fill->TransportLayerASIO::ASIOTicket::finishFill
void TransportLayerASIO::ASIOTicket::finishFill(Status status) { //每一个阶段处理完后都通过这里执行相应的回调
    // We want to make sure that a Ticket can only be filled once; filling a ticket invalidates it.
    // So we check that the _fillCallback is set, then move it out of the ticket and into a local
    // variable, and then call that. It's illegal to interact with the ticket after calling the
    // fillCallback, so we have to move it out of _fillCallback so there are no writes to any
    // variables in ASIOTicket after it gets called.
    invariant(_fillCallback);
    auto fillCallback = std::move(_fillCallback);
    fillCallback(status); //TransportLayerASIO::asyncWait中赋值，可以是ServiceStateMachine::_sourceCallback
}

//ServiceStateMachine::_sourceMessage->TransportLayerASIO::asyncWait->TransportLayerASIO::ASIOTicket::fill->TransportLayerASIO::ASIOTicket::finishFill
void TransportLayerASIO::ASIOTicket::fill(bool sync, TicketCallback&& cb) {
    _fillSync = sync;
    dassert(!_fillCallback);
    _fillCallback = std::move(cb);
    fillImpl();
}

}  // namespace transport
}  // namespace mongo
