// Copyright 2020 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "p2p/protocol.h"
#include "p2p/msg_reader.h"

#include "core/block_crypt.h"   // BbsChannel
#include "core/fly_client.h"    // INetwork
#include "wallet/core/wallet.h" // IWalletMessageEndpoint

#include "interface.h"

namespace beam
{
    /**
     *  Dispatches broadcast messages between network and listeners.
     *  Current implementation uses the specified scope of BBS channels as a tunnel for messages.
     *  Encapsulates transport protocol.
     */
    class BroadcastRouter
        : public IBroadcastMsgGateway
        , IErrorHandler  // Error handling for Protocol
        , proto::FlyClient::IBbsReceiver
    {
    public:
        BroadcastRouter(proto::FlyClient::INetwork&, wallet::IWalletMessageEndpoint&);

        // IBroadcastMsgGateway
        void registerListener(BroadcastContentType, IBroadcastListener*) override;
        void unregisterListener(BroadcastContentType) override;
        void sendRawMessage(BroadcastContentType type, const ByteBuffer&) override; // deprecated. used in SwapOffersBoard. should be made private.
        void sendMessage(BroadcastContentType type, const BroadcastMsg&) override;

        // IBbsReceiver
        virtual void OnMsg(proto::BbsMsg&&) override;

        // IErrorHandler
        virtual void on_protocol_error(uint64_t fromStream, ProtocolError error) override;
        virtual void on_connection_error(uint64_t fromStream, io::ErrorCode errorCode) override; /// unused

    private:
        static constexpr std::array<uint8_t, 3> m_ver_1 = { 0, 0, 1 };  // before 2nd hard fork: has custom deserialization for swap offer board
        static constexpr std::array<uint8_t, 3> m_ver_2 = { 0, 0, 2 };  // after 2nd hard fork: common deserialization for all BBS-based broadcasting
        static constexpr size_t m_maxMessageTypes = 3;
        static constexpr size_t m_defaultMessageSize = 200;         // set experimentally
        static constexpr size_t m_minMessageSize = 1;
        static constexpr size_t m_maxMessageSize = 1024*1024*10;
        static constexpr uint32_t m_bbsTimeWindow = 12*60*60;       // BBS message lifetime is 12 hours

        static const std::vector<BbsChannel> m_incomingBbsChannels;
        static const std::map<BroadcastContentType, BbsChannel> m_outgoingBbsChannelsMap;
        static const std::map<BroadcastContentType, MsgType> m_messageTypeMap;

        MsgType getMsgType(BroadcastContentType);
        BbsChannel getBbsChannel(BroadcastContentType);

        proto::FlyClient::INetwork& m_bbsNetwork;
        wallet::IWalletMessageEndpoint& m_bbsMessageEndpoint;

        Protocol m_protocol_old;
        Protocol m_protocol_new;
        MsgReader m_msgReader_old;
        MsgReader m_msgReader_new;
        Timestamp m_lastTimestamp;
        std::map<BroadcastContentType, IBroadcastListener*> m_listeners;
    };

} // namespace beam
