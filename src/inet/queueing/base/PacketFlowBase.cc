//
// Copyright (C) OpenSim Ltd.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, see http://www.gnu.org/licenses/.
//

#include "inet/common/ModuleAccess.h"
#include "inet/queueing/base/PacketFlowBase.h"

namespace inet {
namespace queueing {

void PacketFlowBase::initialize(int stage)
{
    PacketProcessorBase::initialize(stage);
    if (stage == INITSTAGE_LOCAL) {
        inputGate = gate("in");
        outputGate = gate("out");
        producer = findConnectedModule<IActivePacketSource>(inputGate);
        consumer = findConnectedModule<IPassivePacketSink>(outputGate);
        provider = findConnectedModule<IPassivePacketSource>(inputGate);
        collector = findConnectedModule<IActivePacketSink>(outputGate);
    }
    else if (stage == INITSTAGE_QUEUEING) {
        checkPacketOperationSupport(inputGate);
        checkPacketOperationSupport(outputGate);
    }
}

void PacketFlowBase::handleMessage(cMessage *message)
{
    auto packet = check_and_cast<Packet *>(message);
    pushPacket(packet, packet->getArrivalGate());
}

void PacketFlowBase::checkPacketStreaming(Packet *packet)
{
    if (inProgressStreamId != -1 && (packet == nullptr || packet->getTreeId() != inProgressStreamId))
        throw cRuntimeError("Another packet streaming operation is already in progress");
}

void PacketFlowBase::startPacketStreaming(Packet *packet)
{
    inProgressStreamId = packet->getTreeId();
}

void PacketFlowBase::endPacketStreaming(Packet *packet)
{
    handlePacketProcessed(packet);
    inProgressStreamId = -1;
}

bool PacketFlowBase::canPushSomePacket(cGate *gate) const
{
    return consumer == nullptr || consumer->canPushSomePacket(outputGate->getPathEndGate());
}

bool PacketFlowBase::canPushPacket(Packet *packet, cGate *gate) const
{
    return consumer == nullptr || consumer->canPushPacket(packet, outputGate->getPathEndGate());
}

void PacketFlowBase::pushPacket(Packet *packet, cGate *gate)
{
    Enter_Method("pushPacket");
    take(packet);
    checkPacketStreaming(nullptr);
    processPacket(packet);
    pushOrSendPacket(packet, outputGate, consumer);
    handlePacketProcessed(packet);
    updateDisplayString();
}

void PacketFlowBase::pushPacketStart(Packet *packet, cGate *gate, bps datarate)
{
    Enter_Method("pushPacketStart");
    take(packet);
    checkPacketStreaming(packet);
    startPacketStreaming(packet);
    processPacket(packet);
    pushOrSendPacketStart(packet, outputGate, consumer, datarate);
    updateDisplayString();
}

void PacketFlowBase::pushPacketEnd(Packet *packet, cGate *gate)
{
    Enter_Method("pushPacketEnd");
    take(packet);
    if (!isStreamingPacket())
        startPacketStreaming(packet);
    else
        checkPacketStreaming(packet);
    processPacket(packet);
    emit(packetPushedSignal, packet);
    endPacketStreaming(packet);
    pushOrSendPacketEnd(packet, outputGate, consumer);
    updateDisplayString();
}

void PacketFlowBase::pushPacketProgress(Packet *packet, cGate *gate, bps datarate, b position, b extraProcessableLength)
{
    Enter_Method("pushPacketProgress");
    take(packet);
    if (!isStreamingPacket())
        startPacketStreaming(packet);
    else
        checkPacketStreaming(packet);
    bool isPacketEnd = packet->getTotalLength() == position + extraProcessableLength;
    processPacket(packet);
    if (isPacketEnd) {
        emit(packetPushedSignal, packet);
        endPacketStreaming(packet);
        pushOrSendPacketEnd(packet, outputGate, consumer);
    }
    else
        pushOrSendPacketProgress(packet, outputGate, consumer, datarate, position, extraProcessableLength);
    updateDisplayString();
}

b PacketFlowBase::getPushPacketProcessedLength(Packet *packet, cGate *gate)
{
    checkPacketStreaming(packet);
    return consumer->getPushPacketProcessedLength(packet, outputGate->getPathEndGate());
}

void PacketFlowBase::handleCanPushPacketChanged(cGate *gate)
{
    Enter_Method("handleCanPushPacketChanged");
    if (producer != nullptr)
        producer->handleCanPushPacketChanged(inputGate->getPathStartGate());
}

void PacketFlowBase::handlePushPacketProcessed(Packet *packet, cGate *gate, bool successful)
{
    Enter_Method("handlePushPacketProcessed");
    endPacketStreaming(packet);
    if (producer != nullptr)
        producer->handlePushPacketProcessed(packet, inputGate->getPathStartGate(), successful);
}

bool PacketFlowBase::canPullSomePacket(cGate *gate) const
{
    return provider->canPullSomePacket(inputGate->getPathStartGate());
}

Packet *PacketFlowBase::canPullPacket(cGate *gate) const
{
    return provider->canPullPacket(inputGate->getPathStartGate());
}

Packet *PacketFlowBase::pullPacket(cGate *gate)
{
    Enter_Method("pullPacket");
    checkPacketStreaming(nullptr);
    auto packet = provider->pullPacket(inputGate->getPathStartGate());
    take(packet);
    processPacket(packet);
    handlePacketProcessed(packet);
    animateSendPacket(packet, outputGate);
    updateDisplayString();
    return packet;
}

Packet *PacketFlowBase::pullPacketStart(cGate *gate, bps datarate)
{
    Enter_Method("pullPacketStart");
    checkPacketStreaming(nullptr);
    auto packet = provider->pullPacketStart(inputGate->getPathStartGate(), datarate);
    take(packet);
    inProgressStreamId = packet->getTreeId();
    processPacket(packet);
    animateSendPacketStart(packet, outputGate, datarate);
    updateDisplayString();
    return packet;
}

Packet *PacketFlowBase::pullPacketEnd(cGate *gate)
{
    Enter_Method("pullPacketEnd");
    auto packet = provider->pullPacketEnd(inputGate->getPathStartGate());
    take(packet);
    checkPacketStreaming(packet);
    processPacket(packet);
    inProgressStreamId = packet->getTreeId();
    emit(packetPulledSignal, packet);
    endPacketStreaming(packet);
    animateSendPacketEnd(packet, outputGate);
    updateDisplayString();
    return packet;
}

Packet *PacketFlowBase::pullPacketProgress(cGate *gate, bps datarate, b position, b extraProcessableLength)
{
    Enter_Method("pullPacketProgress");
    auto packet = provider->pullPacketProgress(inputGate->getPathStartGate(), datarate, position, extraProcessableLength);
    take(packet);
    checkPacketStreaming(packet);
    inProgressStreamId = packet->getTreeId();
    bool isPacketEnd = packet->getTotalLength() == position + extraProcessableLength;
    processPacket(packet);
    if (isPacketEnd) {
        emit(packetPulledSignal, packet);
        endPacketStreaming(packet);
    }
    animateSendPacketProgress(packet, outputGate, datarate, position, extraProcessableLength);
    updateDisplayString();
    return packet;
}

b PacketFlowBase::getPullPacketProcessedLength(Packet *packet, cGate *gate)
{
    checkPacketStreaming(packet);
    return provider->getPullPacketProcessedLength(packet, inputGate->getPathStartGate());
}

void PacketFlowBase::handleCanPullPacketChanged(cGate *gate)
{
    Enter_Method("handleCanPullPacketChanged");
    if (collector != nullptr)
        collector->handleCanPullPacketChanged(outputGate->getPathEndGate());
}

void PacketFlowBase::handlePullPacketProcessed(Packet *packet, cGate *gate, bool successful)
{
    Enter_Method("handlePullPacketProcessed");
    endPacketStreaming(packet);
    if (collector != nullptr)
        collector->handlePullPacketProcessed(packet, outputGate->getPathStartGate(), successful);
}

} // namespace queueing
} // namespace inet

