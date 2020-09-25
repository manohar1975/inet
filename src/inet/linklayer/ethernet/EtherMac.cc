//
// Copyright (C) 2003 Andras Varga; CTIE, Monash University, Australia
// Copyright (C) 2011 OpenSim Ltd.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//

#include "inet/common/IProtocolRegistrationListener.h"
#include "inet/common/ProtocolTag_m.h"
#include "inet/linklayer/common/EtherType_m.h"
#include "inet/linklayer/common/Ieee802Ctrl.h"
#include "inet/linklayer/common/InterfaceTag_m.h"
#include "inet/linklayer/common/MacAddressTag_m.h"
#include "inet/linklayer/ethernet/EtherEncap.h"
#include "inet/linklayer/ethernet/EtherMac.h"
#include "inet/linklayer/ethernet/Ethernet.h"
#include "inet/linklayer/ethernet/EthernetControlFrame_m.h"
#include "inet/linklayer/ethernet/EthernetMacHeader_m.h"
#include "inet/networklayer/common/NetworkInterface.h"
#include "inet/physicallayer/ethernet/EthernetSignal_m.h"

namespace inet {

// TODO: there is some code that is pretty much the same as the one found in EtherMacFullDuplex.cc (e.g. EtherMac::beginSendFrames)
// TODO: refactor using a statemachine that is present in a single function
// TODO: this helps understanding what interactions are there and how they affect the state

static std::ostream& operator<<(std::ostream& out, cMessage *msg)
{
    out << "(" << msg->getClassName() << ")" << msg->getFullName();
    return out;
}

static std::ostream& operator<<(std::ostream& out, cPacket *msg)
{
    out << "(" << msg->getClassName() << ")" << msg->getFullName()
        << (msg->isReceptionStart() ? "-start" : msg->isReceptionEnd() ? "-end" : "")
        << " ("
        << (msg->getArrivalTime() + msg->getRemainingDuration() - msg->getDuration()).ustr()
        << ","
        << msg->getArrivalTime().ustr()
        << ","
        << (msg->getArrivalTime() + msg->getRemainingDuration()).ustr()
        << ")"
        ;
    return out;
}

Define_Module(EtherMac);

simsignal_t EtherMac::collisionSignal = registerSignal("collision");
simsignal_t EtherMac::backoffSlotsGeneratedSignal = registerSignal("backoffSlotsGenerated");

EtherMac::~EtherMac()
{
    cancelAndDelete(endBackoffTimer);
}

void EtherMac::initialize(int stage)
{
    EtherMacBase::initialize(stage);

    if (stage == INITSTAGE_LOCAL) {
        endBackoffTimer = new cMessage("EndBackoff", ENDBACKOFF);

        // initialize state info
        backoffs = 0;
        WATCH(backoffs);
    }
}

void EtherMac::initializeStatistics()
{
    EtherMacBase::initializeStatistics();

    framesSentInBurst = 0;
    bytesSentInBurst = B(0);

    WATCH(framesSentInBurst);
    WATCH(bytesSentInBurst);

    // initialize statistics
    totalCollisionTime = 0.0;
    totalSuccessfulRxTxTime = 0.0;
    numCollisions = numBackoffs = 0;

    WATCH(numCollisions);
    WATCH(numBackoffs);
}

void EtherMac::initializeFlags()
{
    EtherMacBase::initializeFlags();

    duplexMode = par("duplexMode");
    frameBursting = !duplexMode && par("frameBursting");
    physInGate->setDeliverImmediately(true);
    setTxUpdateSupport(true);
}

void EtherMac::processConnectDisconnect()
{
    if (!connected) {
        activeReceptionId = -1;
        activeReceptionStart = SIMTIME_ZERO;
        cancelEvent(endBackoffTimer);
        bytesSentInBurst = B(0);
        framesSentInBurst = 0;
    }

    EtherMacBase::processConnectDisconnect();

    if (connected) {
        changeReceptionState(RX_IDLE_STATE);
        changeTransmissionState(TX_IDLE_STATE);
    }
}

void EtherMac::readChannelParameters(bool errorWhenAsymmetric)
{
    EtherMacBase::readChannelParameters(errorWhenAsymmetric);

    if (connected && !duplexMode) {
        if (curEtherDescr->halfDuplexFrameMinBytes < B(0))
            throw cRuntimeError("%g bps Ethernet only supports full-duplex links", curEtherDescr->txrate);
    }
}

void EtherMac::handleSelfMessage(cMessage *msg)
{
    // Process different self-messages (timer signals)
    EV_TRACE << "Self-message " << msg << " received\n";

    switch (msg->getKind()) {
        case ENDIFG:
            handleEndIFGPeriod();
            break;

        case ENDTRANSMISSION:
            handleEndTxPeriod();
            break;

        case ENDBACKOFF:
            handleEndBackoffPeriod();
            break;

        case ENDPAUSE:
            handleEndPausePeriod();
            break;

        default:
            throw cRuntimeError("Self-message with unexpected message kind %d", msg->getKind());
    }
}

void EtherMac::handleMessageWhenUp(cMessage *msg)
{
    if (channelsDiffer)
        readChannelParameters(true);

    printState();

    // some consistency check
    if (!duplexMode && transmitState == TRANSMITTING_STATE && receiveState != RX_IDLE_STATE)
        throw cRuntimeError("Inconsistent state -- transmitting and receiving at the same time");

    if (msg->isSelfMessage())
        handleSelfMessage(msg);
    else if (msg->getArrivalGateId() == upperLayerInGateId)
        handleUpperPacket(check_and_cast<Packet *>(msg));
    else if (msg->getArrivalGate() == physInGate)
        processSignalFromNetwork(check_and_cast<EthernetSignalBase *>(msg));
    else
        throw cRuntimeError("Message received from unknown gate");

    processAtHandleMessageFinished();
    printState();
}

void EtherMac::handleUpperPacket(Packet *packet)
{
    EV_INFO << "Received " << packet << " from upper layer." << endl;

    numFramesFromHL++;
    emit(packetReceivedFromUpperSignal, packet);

    MacAddress address = getMacAddress();

    auto frame = packet->peekAtFront<EthernetMacHeader>();
    if (frame->getDest().equals(address)) {
        throw cRuntimeError("Logic error: frame %s from higher layer has local MAC address as dest (%s)",
                packet->getFullName(), frame->getDest().str().c_str());
    }

    if (packet->getDataLength() > MAX_ETHERNET_FRAME_BYTES) {
        throw cRuntimeError("Packet length from higher layer (%s) exceeds maximum Ethernet frame size (%s)",
                packet->getDataLength().str().c_str(), MAX_ETHERNET_FRAME_BYTES.str().c_str());
    }

    if (!connected) {
        EV_WARN << "Interface is not connected -- dropping packet " << frame << endl;
        PacketDropDetails details;
        details.setReason(INTERFACE_DOWN);
        emit(packetDroppedSignal, packet, &details);
        numDroppedPkFromHLIfaceDown++;
        delete packet;

        return;
    }

    // fill in src address if not set
    if (frame->getSrc().isUnspecified()) {
        //frame is immutable
        frame = nullptr;
        auto newFrame = packet->removeAtFront<EthernetMacHeader>();
        newFrame->setSrc(address);
        packet->insertAtFront(newFrame);
        frame = newFrame;
    }

    addPaddingAndSetFcs(packet, MIN_ETHERNET_FRAME_BYTES);  // calculate valid FCS

    // store frame and possibly begin transmitting
    EV_DETAIL << "Frame " << packet << " arrived from higher layer, enqueueing\n";
    txQueue->enqueuePacket(packet);

    if ((duplexMode || receiveState == RX_IDLE_STATE) && transmitState == TX_IDLE_STATE) {
        EV_DETAIL << "No incoming carrier signals detected, frame clear to send\n";

        if (!currentTxFrame && !txQueue->isEmpty())
            popTxQueue();

        startFrameTransmission();
    }
}

void EtherMac::addReceptionInReconnectState(long packetTreeId, simtime_t endRxTime)
{
}

void EtherMac::processReceivedJam(EthernetSignalBase *jam)
{
    delete jam;
    processDetectedCollision();
}

void EtherMac::processSignalFromNetwork(EthernetSignalBase *signal)
{
    EV_DETAIL << "Received " << signal << (signal->isReceptionStart() ? "-start" : signal->isReceptionEnd() ? "-end" : "") << " from network.\n";

    if (signal->getSrcMacFullDuplex() != duplexMode)
        throw cRuntimeError("Ethernet misconfiguration: MACs on the same link must be all in full duplex mode, or all in half-duplex mode");

    if (!connected) {
        EV_WARN << "Interface is not connected -- dropping msg " << signal << endl;
        if (signal->getRemainingDuration() == SIMTIME_ZERO && dynamic_cast<EthernetSignal *>(signal)) {    // count only the frame ends, do not count JAM and IFG packets
            auto packet = check_and_cast<Packet *>(signal->decapsulate());
            delete signal;
            decapsulate(packet);
            PacketDropDetails details;
            details.setReason(INTERFACE_DOWN);
            emit(packetDroppedSignal, packet, &details);
            delete packet;
            numDroppedIfaceDown++;
        }
        else
            delete signal;

        return;
    }

    // detect cable length violation in half-duplex mode
    if (!duplexMode) {
        simtime_t propagationTime = simTime() - signal->getSendingTime();
        if (propagationTime >= curEtherDescr->maxPropagationDelay) {
            throw cRuntimeError("Very long frame propagation time detected, maybe cable exceeds "
                                "maximum allowed length? (%lgs corresponds to an approx. %lgm cable)",
                    SIMTIME_STR(propagationTime),
                    SIMTIME_STR(propagationTime * SPEED_OF_LIGHT_IN_CABLE));
        }
    }

    auto pkId = signal->getOrigPacketId();
    auto now = simTime();
    if (pkId == -1) {
        // signal start:
        EV_INFO << "Reception of " << signal << " started.\n";
        ASSERT(activeReceptionId == -1);
        activeReceptionId = signal->getId();
        activeReceptionStart = now;
        calculateRxStatus();
        delete signal;
        return;
    }

    // signal update or signal end:
    if (activeReceptionId == -1) {
        // signal start was missed
        EV_WARN << "Reception of " << signal << " start was missed.\n";
        activeReceptionId = pkId;
        activeReceptionStart = now;
    }
    else if (activeReceptionId != pkId)
        throw cRuntimeError("model error: mixed reception arrived");

    calculateRxStatus();

    if (signal->isReceptionEnd()) {
        // signal end
        handleEndRxPeriod(signal);
    }
    else {
        // inside of signal
        EV_INFO << "Reception of " << signal << " modified.\n";
        delete signal;
    }
}

void EtherMac::handleEndRxPeriod(EthernetSignalBase *signal)
{
    // signal end
    EV_INFO << "Reception of " << signal << " finished.\n";
    activeReceptionId = -1;
    simtime_t now = simTime();
    simtime_t dt = now - channelBusySince;
    switch (receiveState) {
        case RECEIVING_STATE:
            ASSERT(now - activeReceptionStart <= signal->getDuration());
            if (now - activeReceptionStart != signal->getDuration()) {
                EV_WARN << "Reception of " << signal << " incomplete, begin of signal was missed, marked as bitError.\n";
                signal->setBitError(true);
            }
            frameReceptionComplete(signal);
            totalSuccessfulRxTxTime += dt;
            break;

        case RX_COLLISION_STATE:
            EV_DETAIL << "Incoming signals finished after collision\n";
            delete signal;
            totalCollisionTime += dt;
            break;

        case RX_RECONNECT_STATE:
            EV_DETAIL << "Incoming signals finished or reconnect time elapsed after reconnect\n";
            delete signal;
            break;

        default:
            throw cRuntimeError("model error: invalid receiveState %d", receiveState);
    }

    calculateRxStatus();

    if (!duplexMode && transmitState == TX_IDLE_STATE) {
        EV_DETAIL << "Start IFG period\n";
        scheduleEndIFGPeriod();
    }
}

void EtherMac::processDetectedCollision()
{
    if (receiveState != RX_COLLISION_STATE) {
        numCollisions++;
        emit(collisionSignal, 1L);
        // go to collision state
        changeReceptionState(RX_COLLISION_STATE);
    }
}

void EtherMac::calculateRxStatus()
{
    MacReceiveState newRxState = receiveState;
    bool hasRx = (activeReceptionId != -1);

    if (duplexMode) {
        newRxState = hasRx ? RECEIVING_STATE : RX_IDLE_STATE;
    }
    else {
        bool hasTx = (transmitState == SEND_IFG_STATE
                || transmitState == TRANSMITTING_STATE
                || transmitState == JAMMING_STATE);

        if (hasRx && hasTx) {
            if (receiveState != RX_COLLISION_STATE) {
                numCollisions++;
                emit(collisionSignal, 1L);
                newRxState = RX_COLLISION_STATE;
                abortTransmissionAndAppendJam();
            }
        }
        else {
            if (!hasRx && !hasTx) {
                newRxState = RX_IDLE_STATE;
                if (receiveState == RX_COLLISION_STATE)
                    emit(collisionSignal, 0L);
            }
            else if (!hasTx && hasRx && receiveState != RX_COLLISION_STATE)
                newRxState = RECEIVING_STATE;
        }
        if (newRxState != receiveState) {
            if (receiveState == RX_IDLE_STATE)
                channelBusySince = simTime();
        }
    }
    if (newRxState != receiveState) {
        changeReceptionState(newRxState);
    }
}

void EtherMac::handleEndIFGPeriod()
{
    if (transmitState != WAIT_IFG_STATE && transmitState != SEND_IFG_STATE)
        throw cRuntimeError("Not in WAIT_IFG_STATE at the end of IFG period");

    EV_DETAIL << "IFG elapsed\n";

    if (frameBursting && (transmitState != SEND_IFG_STATE)) {
        bytesSentInBurst = B(0);
        framesSentInBurst = 0;
    }

    // End of IFG period, okay to transmit, if Rx idle OR duplexMode ( checked in startFrameTransmission(); )

    if (currentTxFrame == nullptr && !txQueue->isEmpty())
        popTxQueue();

    // send frame to network
    beginSendFrames();
}

B EtherMac::calculateMinFrameLength()
{
    bool inBurst = frameBursting && framesSentInBurst;
    B minFrameLength = duplexMode ? MIN_ETHERNET_FRAME_BYTES : (inBurst ? curEtherDescr->frameInBurstMinBytes : curEtherDescr->halfDuplexFrameMinBytes);

    return minFrameLength;
}

B EtherMac::calculatePaddedFrameLength(Packet *frame)
{
    B minFrameLength = calculateMinFrameLength();
    return std::max(minFrameLength, B(frame->getDataLength()));
}

void EtherMac::startFrameTransmission()
{
    ASSERT(currentTxFrame);
    EV_DETAIL << "Transmitting a copy of frame " << currentTxFrame << endl;

    Packet *frame = currentTxFrame->dup();

    const auto& hdr = frame->peekAtFront<EthernetMacHeader>();
    ASSERT(hdr);
    ASSERT(!hdr->getSrc().isUnspecified());

    B minFrameLengthWithExtension = calculateMinFrameLength();
    B extensionLength = minFrameLengthWithExtension > frame->getDataLength() ? (minFrameLengthWithExtension - frame->getDataLength()) : B(0);

    // add preamble and SFD (Starting Frame Delimiter), then send out
    encapsulate(frame);

    B sentFrameByteLength = frame->getDataLength() + extensionLength;
    auto& oldPacketProtocolTag = frame->removeTag<PacketProtocolTag>();
    frame->clearTags();
    auto newPacketProtocolTag = frame->addTag<PacketProtocolTag>();
    *newPacketProtocolTag = *oldPacketProtocolTag;
    EV_INFO << "Transmission of " << frame << " started.\n";
    auto signal = new EthernetSignal(frame->getName());
    signal->setSrcMacFullDuplex(duplexMode);
    signal->setBitrate(curEtherDescr->txrate);
    if (sendRawBytes) {
        auto bytes = frame->peekDataAsBytes();
        frame->eraseAll();
        frame->insertAtFront(bytes);
    }
    signal->encapsulate(frame);
    signal->addByteLength(extensionLength.get());
    curTxSignal = signal->dup();
    curTxSignal->setOrigPacketId(signal->getId());
    simtime_t duration = signal->getBitLength() / curEtherDescr->txrate;
    send(signal, SendOptions().duration(duration), physOutGate);
    scheduleEndTxPeriod(sentFrameByteLength);
    // only count transmissions in totalSuccessfulRxTxTime if channel is half-duplex
    if (!duplexMode)
        channelBusySince = simTime();

    // check for collisions (there might be an ongoing reception which we don't know about, see below)
    // During the IFG period the hardware cannot listen to the channel,
    // so it might happen that receptions have begun during the IFG,
    // and even collisions might be in progress.
    //
    // But we don't know of any ongoing transmission so we blindly
    // start transmitting, immediately collide and send a jam signal.
    //
    calculateRxStatus();
    printState();
}

void EtherMac::abortTransmissionAndAppendJam()
{
    ASSERT(curTxSignal != nullptr);
    cMessage *txTimer = endTxTimer;
    simtime_t startTransmissionTime = txTimer->getSendingTime();
    simtime_t sentDuration = simTime() - startTransmissionTime;
    double sentPart = sentDuration / (txTimer->getArrivalTime() - startTransmissionTime);
    int64_t oldBitLength = curTxSignal->getBitLength();
    int64_t newBitLength = ceil(oldBitLength * sentPart);
    if (auto curTxPacket = check_and_cast_nullable<Packet*>(curTxSignal->decapsulate())) {
        //TODO: removed length calculation based on the PHY layer (parallel bits, bit order, etc.)
        if (newBitLength < curTxPacket->getBitLength()) {
            curTxPacket->trimFront();
            curTxPacket->setBackOffset(b(newBitLength));
            curTxPacket->trimBack();
            curTxPacket->setBitError(true);
        }
        curTxSignal->encapsulate(curTxPacket);
    }
    curTxSignal->setBitLength(newBitLength);
    curTxSignal->addByteLength(JAM_SIGNAL_BYTES.get()); // append JAM
    curTxSignal->setBitError(true);
    simtime_t duration = curTxSignal->getBitLength() / curEtherDescr->txrate;
    send(curTxSignal->dup(), SendOptions().updateTx(curTxSignal->getOrigPacketId()).duration(duration), physOutGate);
    rescheduleAt(transmissionChannel->getTransmissionFinishTime(), txTimer);
    changeTransmissionState(JAMMING_STATE);
}

void EtherMac::handleEndJammingPeriod()
{
    EV_DETAIL << "Jamming finished, executing backoff\n";
    handleRetransmission();
}

void EtherMac::handleEndTxPeriod()
{
    if (curTxSignal == nullptr)
        throw cRuntimeError("Frame under transmission cannot be found");

    simtime_t sentDuration = simTime() - curTxSignal->getCreationTime();    // curTxSignal created at original send();
    send(curTxSignal, SendOptions().finishTx(curTxSignal->getOrigPacketId()).duration(sentDuration), physOutGate);
    curTxSignal = nullptr;

    if (transmitState == SEND_IFG_STATE) {
        handleEndIFGPeriod();
        return;
    }

    if (transmitState == JAMMING_STATE) {
        handleEndJammingPeriod();
        return;
    }

    // we only get here if transmission has finished successfully, without collision
    if (transmitState != TRANSMITTING_STATE || (!duplexMode && receiveState != RX_IDLE_STATE))
        throw cRuntimeError("End of transmission, and incorrect state detected");

    numFramesSent++;
    numBytesSent += currentTxFrame->getByteLength();
    emit(packetSentToLowerSignal, currentTxFrame);    //consider: emit with start time of frame

    const auto& header = currentTxFrame->peekAtFront<EthernetMacHeader>();
    if (header->getTypeOrLength() == ETHERTYPE_FLOW_CONTROL) {
        const auto& controlFrame = currentTxFrame->peekDataAt<EthernetControlFrameBase>(header->getChunkLength(), b(-1));
        if (controlFrame->getOpCode() == ETHERNET_CONTROL_PAUSE) {
            const auto& pauseFrame = dynamicPtrCast<const EthernetPauseFrame>(controlFrame);
            numPauseFramesSent++;
            emit(txPausePkUnitsSignal, pauseFrame->getPauseTime());
        }
    }

    EV_INFO << "Transmission of " << currentTxFrame << " successfully completed.\n";
    deleteCurrentTxFrame();
    lastTxFinishTime = simTime();
    // note: cannot be moved into handleEndIFGPeriod(), because in burst mode we need to know whether to send filled IFG or not
    if (!duplexMode && frameBursting && framesSentInBurst > 0 && !txQueue->isEmpty())
        popTxQueue();

    // only count transmissions in totalSuccessfulRxTxTime if channel is half-duplex
    if (!duplexMode) {
        simtime_t dt = simTime() - channelBusySince;
        totalSuccessfulRxTxTime += dt;
    }

    backoffs = 0;

    // check for and obey received PAUSE frames after each transmission
    if (pauseUnitsRequested > 0) {
        // if we received a PAUSE frame recently, go into PAUSE state
        EV_DETAIL << "Going to PAUSE mode for " << pauseUnitsRequested << " time units\n";
        scheduleEndPausePeriod(pauseUnitsRequested);
        pauseUnitsRequested = 0;
    }
    else {
        EV_DETAIL << "Start IFG period\n";
        scheduleEndIFGPeriod();
        fillIFGIfInBurst();
    }
}

void EtherMac::handleEndBackoffPeriod()
{
    if (transmitState != BACKOFF_STATE)
        throw cRuntimeError("At end of BACKOFF and not in BACKOFF_STATE");

    if (currentTxFrame == nullptr)
        throw cRuntimeError("At end of BACKOFF and no frame to transmit");

    if (receiveState == RX_IDLE_STATE) {
        EV_DETAIL << "Backoff period ended, wait IFG\n";
        scheduleEndIFGPeriod();
    }
    else {
        EV_DETAIL << "Backoff period ended but channel is not free, idling\n";
        changeTransmissionState(TX_IDLE_STATE);
    }
}

void EtherMac::handleRetransmission()
{
    if (++backoffs > MAX_ATTEMPTS) {
        EV_DETAIL << "Number of retransmit attempts of frame exceeds maximum, cancelling transmission of frame\n";
        PacketDropDetails details;
        details.setReason(RETRY_LIMIT_REACHED);
        details.setLimit(MAX_ATTEMPTS);
        dropCurrentTxFrame(details);
        backoffs = 0;
        if (!txQueue->isEmpty())
            popTxQueue();
        tryBeginSendFrame();
        return;
    }

    int backoffRange = (backoffs >= BACKOFF_RANGE_LIMIT) ? 1024 : (1 << backoffs);
    int slotNumber = intuniform(0, backoffRange - 1);
    EV_DETAIL << "Executing backoff procedure (slotNumber=" << slotNumber << ", backoffRange=[0," << backoffRange -1 << "]" << endl;

    scheduleAfter(slotNumber * curEtherDescr->slotTime, endBackoffTimer);
    changeTransmissionState(BACKOFF_STATE);
    emit(backoffSlotsGeneratedSignal, slotNumber);

    numBackoffs++;
}

void EtherMac::printState()
{
#define CASE(x)    case x: \
        EV_DETAIL << #x; break

    EV_DETAIL << "transmitState: ";
    switch (transmitState) {
        CASE(TX_IDLE_STATE);
        CASE(WAIT_IFG_STATE);
        CASE(SEND_IFG_STATE);
        CASE(TRANSMITTING_STATE);
        CASE(JAMMING_STATE);
        CASE(BACKOFF_STATE);
        CASE(PAUSE_STATE);
    }

    EV_DETAIL << ",  receiveState: ";
    switch (receiveState) {
        CASE(RX_IDLE_STATE);
        CASE(RECEIVING_STATE);
        CASE(RX_COLLISION_STATE);
        CASE(RX_RECONNECT_STATE);
    }

    EV_DETAIL << ",  backoffs: " << backoffs;
    EV_DETAIL << ",  queueLength: " << txQueue->getNumPackets();
    EV_DETAIL << endl;

#undef CASE
}

void EtherMac::finish()
{
    EtherMacBase::finish();

    simtime_t t = simTime();
    simtime_t totalChannelIdleTime = t - totalSuccessfulRxTxTime - totalCollisionTime;
    recordScalar("rx channel idle (%)", 100 * (totalChannelIdleTime / t));
    recordScalar("rx channel utilization (%)", 100 * (totalSuccessfulRxTxTime / t));
    recordScalar("rx channel collision (%)", 100 * (totalCollisionTime / t));
    recordScalar("collisions", numCollisions);
    recordScalar("backoffs", numBackoffs);
}

void EtherMac::tryBeginSendFrame()
{
    if (duplexMode)
        beginSendFrames();
    else if (receiveState == RX_IDLE_STATE) {
        EV_DETAIL << "Start IFG period\n";
        scheduleEndIFGPeriod();
    }
    else {
        EV_DETAIL << "channel is not free, idling\n";
        changeTransmissionState(TX_IDLE_STATE);
    }
}

void EtherMac::handleEndPausePeriod()
{
    if (transmitState != PAUSE_STATE)
        throw cRuntimeError("At end of PAUSE and not in PAUSE_STATE");

    EV_DETAIL << "Pause finished, resuming transmissions\n";
    tryBeginSendFrame();
}

void EtherMac::frameReceptionComplete(EthernetSignalBase *signal)
{
    if (dynamic_cast<EthernetFilledIfgSignal *>(signal) != nullptr) {
        delete signal;
        return;
    }
    if (signal->getSrcMacFullDuplex() != duplexMode)
        throw cRuntimeError("Ethernet misconfiguration: MACs on the same link must be all in full duplex mode, or all in half-duplex mode");

    bool hasBitError = signal->hasBitError();
    auto packet = check_and_cast_nullable<Packet *>(signal->decapsulate());

    if (packet == nullptr) {
        if (hasBitError) {
            numDroppedBitError++;
            delete signal;
            return;
        }
        else
            throw cRuntimeError("Model error: Signal %s arrived without content Packet", signal->getName());
    }
    delete signal;

    if (!decapsulate(packet)) {
        numDroppedBitError++;
        PacketDropDetails details;
        details.setReason(INCORRECTLY_RECEIVED);
        emit(packetDroppedSignal, packet, &details);
        delete packet;
        return;
    }

    emit(packetReceivedFromLowerSignal, packet);

    if (hasBitError || !verifyCrcAndLength(packet)) {
        numDroppedBitError++;
        PacketDropDetails details;
        details.setReason(INCORRECTLY_RECEIVED);
        emit(packetDroppedSignal, packet, &details);
        delete packet;
        return;
    }

    const auto& frame = packet->peekAtFront<EthernetMacHeader>();
    auto macAddressInd = packet->addTag<MacAddressInd>();
    macAddressInd->setSrcAddress(frame->getSrc());
    macAddressInd->setDestAddress(frame->getDest());
    if (dropFrameNotForUs(packet, frame))
        return;

    if (frame->getTypeOrLength() == ETHERTYPE_FLOW_CONTROL) {
        processReceivedControlFrame(packet);
    }
    else {
        EV_INFO << "Reception of " << packet << " successfully completed.\n";
        processReceivedDataFrame(packet);
    }
}

void EtherMac::processReceivedDataFrame(Packet *packet)
{
    // statistics
    unsigned long curBytes = packet->getByteLength();
    numFramesReceivedOK++;
    numBytesReceivedOK += curBytes;
    emit(rxPkOkSignal, packet);

    packet->addTagIfAbsent<DispatchProtocolReq>()->setProtocol(&Protocol::ethernetMac);
    packet->addTagIfAbsent<PacketProtocolTag>()->setProtocol(&Protocol::ethernetMac);
    if (networkInterface)
        packet->addTagIfAbsent<InterfaceInd>()->setInterfaceId(networkInterface->getInterfaceId());

    numFramesPassedToHL++;
    emit(packetSentToUpperSignal, packet);
    // pass up to upper layer
    EV_INFO << "Sending " << packet << " to upper layer.\n";
    send(packet, "upperLayerOut");
}

void EtherMac::processReceivedControlFrame(Packet *packet)
{
    packet->popAtFront<EthernetMacHeader>();
    const auto& controlFrame = packet->peekAtFront<EthernetControlFrameBase>();

    if (controlFrame->getOpCode() == ETHERNET_CONTROL_PAUSE) {
        const auto& pauseFrame = packet->peekAtFront<EthernetPauseFrame>();
        int pauseUnits = pauseFrame->getPauseTime();

        numPauseFramesRcvd++;
        emit(rxPausePkUnitsSignal, pauseUnits);

        if (transmitState == TX_IDLE_STATE) {
            EV_DETAIL << "PAUSE frame received, pausing for " << pauseUnitsRequested << " time units\n";
            if (pauseUnits > 0)
                scheduleEndPausePeriod(pauseUnits);
        }
        else if (transmitState == PAUSE_STATE) {
            EV_DETAIL << "PAUSE frame received, pausing for " << pauseUnitsRequested
                      << " more time units from now\n";
            cancelEvent(endPauseTimer);

            if (pauseUnits > 0)
                scheduleEndPausePeriod(pauseUnits);
        }
        else {
            // transmitter busy -- wait until it finishes with current frame (endTx)
            // and then it'll go to PAUSE state
            EV_DETAIL << "PAUSE frame received, storing pause request\n";
            pauseUnitsRequested = pauseUnits;
        }
    }
    delete packet;
}

void EtherMac::scheduleEndIFGPeriod()
{
    changeTransmissionState(WAIT_IFG_STATE);
    simtime_t endIFGTime = simTime() + (b(INTERFRAME_GAP_BITS).get() / curEtherDescr->txrate);
    scheduleAt(endIFGTime, endIfgTimer);
}

void EtherMac::fillIFGIfInBurst()
{
    if (!frameBursting)
        return;

    EV_TRACE << "fillIFGIfInBurst(): t=" << simTime() << ", framesSentInBurst=" << framesSentInBurst << ", bytesSentInBurst=" << bytesSentInBurst << endl;

    if (currentTxFrame
        && endIfgTimer->isScheduled()
        && (transmitState == WAIT_IFG_STATE)
        && (simTime() == lastTxFinishTime)
        && (simTime() == endIfgTimer->getSendingTime())
        && (framesSentInBurst > 0)
        && (framesSentInBurst < curEtherDescr->maxFramesInBurst)
        && (bytesSentInBurst + INTERFRAME_GAP_BITS + PREAMBLE_BYTES + SFD_BYTES + calculatePaddedFrameLength(currentTxFrame)
            <= curEtherDescr->maxBytesInBurst)
        )
    {
        EthernetFilledIfgSignal *gap = new EthernetFilledIfgSignal("FilledIFG");
        gap->setBitrate(curEtherDescr->txrate);
        bytesSentInBurst += B(gap->getByteLength());
        curTxSignal = gap->dup();
        curTxSignal->setOrigPacketId(gap->getId());
        simtime_t duration = gap->getBitLength() / curEtherDescr->txrate;
        send(gap, SendOptions().duration(duration), physOutGate);
        changeTransmissionState(SEND_IFG_STATE);
        cancelEvent(endIfgTimer);
        rescheduleAt(transmissionChannel->getTransmissionFinishTime(), endTxTimer);
    }
    else {
        bytesSentInBurst = B(0);
        framesSentInBurst = 0;
    }
}

void EtherMac::scheduleEndTxPeriod(B sentFrameByteLength)
{
    // update burst variables
    if (frameBursting) {
        bytesSentInBurst += sentFrameByteLength;
        framesSentInBurst++;
    }

    scheduleAt(transmissionChannel->getTransmissionFinishTime(), endTxTimer);
    changeTransmissionState(TRANSMITTING_STATE);
}

void EtherMac::scheduleEndPausePeriod(int pauseUnits)
{
    // length is interpreted as 512-bit-time units
    simtime_t pausePeriod = pauseUnits * PAUSE_UNIT_BITS / curEtherDescr->txrate;
    scheduleAfter(pausePeriod, endPauseTimer);
    changeTransmissionState(PAUSE_STATE);
}

void EtherMac::beginSendFrames()
{
    if (currentTxFrame) {
        // Other frames are queued, therefore wait IFG period and transmit next frame
        EV_DETAIL << "Will transmit next frame in output queue after IFG period\n";
        startFrameTransmission();
    }
    else {
        // No more frames, set transmitter to idle
        changeTransmissionState(TX_IDLE_STATE);
        EV_DETAIL << "No more frames to send, transmitter set to idle\n";
    }
}

} // namespace inet

