/*
 * Copyright (C) 2014 Pavel Kirienko <pavel.kirienko@gmail.com>
 */

#include <uavcan/transport/dispatcher.hpp>
#include <uavcan/debug.hpp>
#include <cassert>

namespace uavcan
{
#if !UAVCAN_TINY
/*
 * LoopbackFrameListenerBase
 */
void LoopbackFrameListenerBase::startListening()
{
    dispatcher_.getLoopbackFrameListenerRegistry().add(this);
}

void LoopbackFrameListenerBase::stopListening()
{
    dispatcher_.getLoopbackFrameListenerRegistry().remove(this);
}

bool LoopbackFrameListenerBase::isListening() const
{
    return dispatcher_.getLoopbackFrameListenerRegistry().doesExist(this);
}

/*
 * LoopbackFrameListenerRegistry
 */
void LoopbackFrameListenerRegistry::add(LoopbackFrameListenerBase* listener)
{
    UAVCAN_ASSERT(listener);
    listeners_.insert(listener);
}

void LoopbackFrameListenerRegistry::remove(LoopbackFrameListenerBase* listener)
{
    UAVCAN_ASSERT(listener);
    listeners_.remove(listener);
}

bool LoopbackFrameListenerRegistry::doesExist(const LoopbackFrameListenerBase* listener) const
{
    UAVCAN_ASSERT(listener);
    const LoopbackFrameListenerBase* p = listeners_.get();
    while (p)
    {
        if (p == listener)
        {
            return true;
        }
        p = p->getNextListNode();
    }
    return false;
}

void LoopbackFrameListenerRegistry::invokeListeners(RxFrame& frame)
{
    LoopbackFrameListenerBase* p = listeners_.get();
    while (p)
    {
        LoopbackFrameListenerBase* const next = p->getNextListNode();
        p->handleLoopbackFrame(frame);     // p may be modified
        p = next;
    }
}
#endif

/*
 * Dispatcher::ListenerRegister
 */
bool Dispatcher::ListenerRegistry::add(TransferListener* listener, Mode mode)
{
    if (mode == UniqueListener)
    {
        TransferListener* p = list_.get();
        while (p)
        {
            if (p->getDataTypeDescriptor().getID() == listener->getDataTypeDescriptor().getID())
            {
                return false;
            }
            p = p->getNextListNode();
        }
    }
    // Objective is to arrange entries by Data Type ID in ascending order from root.
    list_.insertBefore(listener, DataTypeIDInsertionComparator(listener->getDataTypeDescriptor().getID()));
    return true;
}

void Dispatcher::ListenerRegistry::remove(TransferListener* listener)
{
    list_.remove(listener);
}

bool Dispatcher::ListenerRegistry::exists(DataTypeID dtid) const
{
    TransferListener* p = list_.get();
    while (p)
    {
        if (p->getDataTypeDescriptor().getID() == dtid)
        {
            return true;
        }
        p = p->getNextListNode();
    }
    return false;
}

void Dispatcher::ListenerRegistry::cleanup(MonotonicTime ts)
{
    TransferListener* p = list_.get();
    while (p)
    {
        TransferListener* const next = p->getNextListNode();
        p->cleanup(ts); // p may be modified
        p = next;
    }
}

void Dispatcher::ListenerRegistry::handleFrame(const RxFrame& frame)
{
    TransferListener* p = list_.get();
    while (p)
    {
        TransferListener* const next = p->getNextListNode();
        if (p->getDataTypeDescriptor().getID() == frame.getDataTypeID())
        {
            p->handleFrame(frame); // p may be modified
        }
        else if (p->getDataTypeDescriptor().getID() < frame.getDataTypeID())  // Listeners are ordered by data type id!
        {
            break;
        }
        else
        {
            ;  // Nothing to do with this one
        }
        p = next;
    }
}

/*
 * Dispatcher
 */
void Dispatcher::handleFrame(const CanRxFrame& can_frame)
{
    RxFrame frame;
    if (!frame.parse(can_frame))
    {
        // This is not counted as a transport error
        UAVCAN_TRACE("Dispatcher", "Invalid CAN frame received: %s", can_frame.toString().c_str());
        return;
    }

    if ((frame.getDstNodeID() != NodeID::Broadcast) &&
        (frame.getDstNodeID() != getNodeID()))
    {
        return;
    }

    switch (frame.getTransferType())
    {
    case TransferTypeMessageBroadcast:
    {
        lmsg_.handleFrame(frame);
        break;
    }
    case TransferTypeServiceRequest:
    {
        lsrv_req_.handleFrame(frame);
        break;
    }
    case TransferTypeServiceResponse:
    {
        lsrv_resp_.handleFrame(frame);
        break;
    }
    default:
    {
        UAVCAN_ASSERT(0);
        break;
    }
    }
    // ADDED SMART BATTERY PARSING HERE
    if (frame.getSrcNodeID() == 0x16) {
      if (parseBatteryFrame(frame)) {
        updateSmartBatteryPayloadMsg();
        publishSmartBatteryPayloadMsg();
      }
    }
}

// ----- BEGIN Smart Battery methods -----

bool Dispatcher::parseBatteryFrame(RxFrame frame) {
    // Returns true if this frame is the end of a transfer
    // and the CRC is valid.
    // Returns false otherwise.
    unsigned payload_length = frame.getPayloadLen();
    const uint8_t* payload_ptr = frame.getPayloadPtr();
    bool valid_frame = false;
    unsigned payload_index_offset = 0;
    bool end_of_msg = false;
    if (frame.isStartOfTransfer()) {
        valid_frame = true;
        previous_battery_frame_toggle_bit_ = false;
        // The first two bytes of the frame are the checksum
        // The remaining bytes are the payload
        battery_msg_crc_ = payload_ptr[0] + (payload_ptr[1] << 8);
        battery_msg_buffer_index_ = 0;
        payload_index_offset = 2;
        payload_length -= 2;
    }
    else if (frame.isEndOfTransfer()) {
        valid_frame = true;
        end_of_msg = true;
    }
    else if (previous_battery_frame_toggle_bit_ != frame.getToggle()) {
        valid_frame = true;
        previous_battery_frame_toggle_bit_ = !previous_battery_frame_toggle_bit_;
    }
    else {
        // The frame is out of place. Reset and wait for another start of transfer.
        previous_battery_frame_toggle_bit_ = false;
    }
    // Populate the buffer with the new frame data
    if (valid_frame) {
        for (unsigned i = 0; i < payload_length; ++i) {
            battery_msg_buffer_[i + battery_msg_buffer_index_] = payload_ptr[i + payload_index_offset];
        }
        battery_msg_buffer_index_ += payload_length;
    }

    if (end_of_msg && isChecksumValid()) {
        return true;
    }
    return false;
}

bool Dispatcher::isChecksumValid() {
  uint16_t checksum_calc_value = 0xFFFF;
  unsigned buffer_index = 0;
  while (buffer_index < BATTERY_MSG_LEN) {
    checksum_calc_value = crcCalculateNext(checksum_calc_value, battery_msg_buffer_[buffer_index]);
    buffer_index++;
  }
  return (battery_msg_crc_ == checksum_calc_value);
}

uint16_t Dispatcher::crcCalculateNext(uint16_t current_value, uint8_t next_byte) {
  current_value ^= static_cast<uint8_t>(next_byte) << 8U;
  for (uint8_t bit = 8; bit > 0; --bit) {
    if ((current_value & 0x8000U) != 0) {
      current_value = (current_value << 1) ^ 0x1021U;
    }
    else {
      current_value = current_value << 1U;
    }
  }
  return current_value;
}

void Dispatcher::updateSmartBatteryPayloadMsg() {
  smart_battery_payload_.timestamp = hrt_absolute_time();
  for(int i=0; i<BATTERY_MSG_LEN; ++i) {
    smart_battery_payload_.payload[i] = battery_msg_buffer_[i];
  }
}

void Dispatcher::publishSmartBatteryPayloadMsg() {
    if (smart_battery_payload_publisher_ != nullptr) {
      orb_publish(ORB_ID(smart_battery_payload), smart_battery_payload_publisher_, &smart_battery_payload_);
    } else {
      smart_battery_payload_publisher_ = orb_advertise(ORB_ID(smart_battery_payload), &smart_battery_payload_);
    }
}

// ----- END Smart Battery methods -----
#if UAVCAN_TINY
void Dispatcher::handleLoopbackFrame(const CanRxFrame&)
{
}

void Dispatcher::notifyRxFrameListener(const CanRxFrame&, CanIOFlags)
{
}
#else
void Dispatcher::handleLoopbackFrame(const CanRxFrame& can_frame)
{
    RxFrame frame;
    if (!frame.parse(can_frame))
    {
        UAVCAN_TRACE("Dispatcher", "Invalid loopback CAN frame: %s", can_frame.toString().c_str());
        UAVCAN_ASSERT(0);  // No way!
        return;
    }
    UAVCAN_ASSERT(frame.getSrcNodeID() == getNodeID());
    loopback_listeners_.invokeListeners(frame);
}

void Dispatcher::notifyRxFrameListener(const CanRxFrame& can_frame, CanIOFlags flags)
{
    if (rx_listener_ != UAVCAN_NULLPTR)
    {
        rx_listener_->handleRxFrame(can_frame, flags);
    }
}
#endif

int Dispatcher::spin(MonotonicTime deadline)
{
    int num_frames_processed = 0;
    do
    {
        CanIOFlags flags = 0;
        CanRxFrame frame;
        const int res = canio_.receive(frame, deadline, flags);
        if (res < 0)
        {
            return res;
        }
        if (res > 0)
        {
            if (flags & CanIOFlagLoopback)
            {
                handleLoopbackFrame(frame);
            }
            else
            {
                num_frames_processed++;
                handleFrame(frame);
            }
            notifyRxFrameListener(frame, flags);
        }
    }
    while (sysclock_.getMonotonic() < deadline);

    return num_frames_processed;
}

int Dispatcher::spinOnce()
{
    int num_frames_processed = 0;

    while (true)
    {
        CanIOFlags flags = 0;
        CanRxFrame frame;
        const int res = canio_.receive(frame, MonotonicTime(), flags);
        if (res < 0)
        {
            return res;
        }
        else if (res > 0)
        {
            if (flags & CanIOFlagLoopback)
            {
                handleLoopbackFrame(frame);
            }
            else
            {
                num_frames_processed++;
                handleFrame(frame);
            }
            notifyRxFrameListener(frame, flags);
        }
        else
        {
            break;      // No frames left
        }
    }

    return num_frames_processed;
}

int Dispatcher::send(const Frame& frame, MonotonicTime tx_deadline, MonotonicTime blocking_deadline,
                     CanTxQueue::Qos qos, CanIOFlags flags, uint8_t iface_mask)
{
    if (frame.getSrcNodeID() != getNodeID())
    {
        UAVCAN_ASSERT(0);
        return -ErrLogic;
    }

    CanFrame can_frame;
    if (!frame.compile(can_frame))
    {
        UAVCAN_TRACE("Dispatcher", "Unable to send: frame is malformed: %s", frame.toString().c_str());
        UAVCAN_ASSERT(0);
        return -ErrLogic;
    }
    return canio_.send(can_frame, tx_deadline, blocking_deadline, iface_mask, qos, flags);
}

void Dispatcher::cleanup(MonotonicTime ts)
{
    outgoing_transfer_reg_.cleanup(ts);
    lmsg_.cleanup(ts);
    lsrv_req_.cleanup(ts);
    lsrv_resp_.cleanup(ts);
}

bool Dispatcher::registerMessageListener(TransferListener* listener)
{
    if (listener->getDataTypeDescriptor().getKind() != DataTypeKindMessage)
    {
        UAVCAN_ASSERT(0);
        return false;
    }
    return lmsg_.add(listener, ListenerRegistry::ManyListeners);       // Multiple subscribers are OK
}

bool Dispatcher::registerServiceRequestListener(TransferListener* listener)
{
    if (listener->getDataTypeDescriptor().getKind() != DataTypeKindService)
    {
        UAVCAN_ASSERT(0);
        return false;
    }
    return lsrv_req_.add(listener, ListenerRegistry::UniqueListener);  // Only one server per data type
}

bool Dispatcher::registerServiceResponseListener(TransferListener* listener)
{
    if (listener->getDataTypeDescriptor().getKind() != DataTypeKindService)
    {
        UAVCAN_ASSERT(0);
        return false;
    }
    return lsrv_resp_.add(listener, ListenerRegistry::ManyListeners);  // Multiple callers may call same srv
}

void Dispatcher::unregisterMessageListener(TransferListener* listener)
{
    lmsg_.remove(listener);
}

void Dispatcher::unregisterServiceRequestListener(TransferListener* listener)
{
    lsrv_req_.remove(listener);
}

void Dispatcher::unregisterServiceResponseListener(TransferListener* listener)
{
    lsrv_resp_.remove(listener);
}

bool Dispatcher::hasSubscriber(DataTypeID dtid) const
{
    return lmsg_.exists(dtid);
}

bool Dispatcher::hasPublisher(DataTypeID dtid) const
{
    return outgoing_transfer_reg_.exists(dtid, TransferTypeMessageBroadcast);
}

bool Dispatcher::hasServer(DataTypeID dtid) const
{
    return lsrv_req_.exists(dtid);
}

bool Dispatcher::setNodeID(NodeID nid)
{
    if (!self_node_id_is_set_)
    {
        self_node_id_ = nid;
        self_node_id_is_set_ = true;
        return true;
    }
    return false;
}

}
