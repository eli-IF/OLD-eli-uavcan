/*
 * Copyright (C) 2014 Pavel Kirienko <pavel.kirienko@gmail.com>
 */

#pragma once

#include <cstdlib>
#include <cassert>
#include <uavcan/data_type.hpp>
#include <uavcan/transport/crc.hpp>
#include <uavcan/transport/transfer.hpp>
#include <uavcan/transport/dispatcher.hpp>

namespace uavcan
{

class TransferSender
{
    const MonotonicDuration max_transfer_interval_;
    const DataTypeDescriptor& data_type_;
    const CanTxQueue::Qos qos_;
    const TransferCRC crc_base_;

    Dispatcher& dispatcher_;

public:
    static MonotonicDuration getDefaultMaxTransferInterval()
    {
        return MonotonicDuration::fromMSec(60 * 1000);
    }

    TransferSender(Dispatcher& dispatcher, const DataTypeDescriptor& data_type, CanTxQueue::Qos qos,
                   MonotonicDuration max_transfer_interval = getDefaultMaxTransferInterval())
    : max_transfer_interval_(max_transfer_interval)
    , data_type_(data_type)
    , qos_(qos)
    , crc_base_(data_type.getSignature().toTransferCRC())
    , dispatcher_(dispatcher)
    { }

    /**
     * Send with explicit Transfer ID.
     * Should be used only for service responses, where response TID should match request TID.
     */
    int send(const uint8_t* payload, int payload_len, MonotonicTime tx_deadline,
             MonotonicTime blocking_deadline, TransferType transfer_type, NodeID dst_node_id,
             TransferID tid);

    /**
     * Send with automatic Transfer ID.
     * TID is managed by OutgoingTransferRegistry.
     */
    int send(const uint8_t* payload, int payload_len, MonotonicTime tx_deadline,
             MonotonicTime blocking_deadline, TransferType transfer_type, NodeID dst_node_id);
};

}