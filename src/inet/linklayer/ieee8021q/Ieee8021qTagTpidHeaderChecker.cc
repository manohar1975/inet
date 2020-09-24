//
// Copyright (C) 2020 OpenSim Ltd.
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

#include <algorithm>
#include "inet/common/ProtocolTag_m.h"
#include "inet/linklayer/common/UserPriorityTag_m.h"
#include "inet/linklayer/common/VlanTag_m.h"
#include "inet/linklayer/ethernet/EtherFrame_m.h"
#include "inet/linklayer/ieee8021q/Ieee8021qChecker.h"

namespace inet {

Define_Module(Ieee8021qChecker);

void Ieee8021qChecker::initialize(int stage)
{
    PacketFilterBase::initialize(stage);
    if (stage == INITSTAGE_LOCAL) {
        const char *vlanTagType = par("vlanTagType");
        if (!strcmp("s", vlanTagType))
            etherType = 0x88A8;
        else if (!strcmp("c", vlanTagType))
            etherType = 0x8100;
        else
            throw cRuntimeError("Unknown tag type");
        cStringTokenizer filterTokenizer(par("vlanIdFilter"));
        while (filterTokenizer.hasMoreTokens())
            vlanIdFilter.push_back(atoi(filterTokenizer.nextToken()));
        WATCH_VECTOR(vlanIdFilter);
    }
}

void Ieee8021qChecker::processPacket(Packet *packet)
{
    const auto& vlanHeader = packet->popAtFront<Ieee8021qHeader>();
    packet->addTagIfAbsent<UserPriorityInd>()->setUserPriority(vlanHeader->getPcp());
    packet->addTagIfAbsent<VlanInd>()->setVlanId(vlanHeader->getVid());
    auto packetProtocolTag = packet->addTagIfAbsent<PacketProtocolTag>();
    packetProtocolTag->setFrontOffset(packetProtocolTag->getFrontOffset() - vlanHeader->getChunkLength());
}

void Ieee8021qChecker::dropPacket(Packet *packet)
{
    EV_WARN << "Received packet " << packet->getName() << " is not accepted, dropping packet.\n";
    PacketFilterBase::dropPacket(packet, OTHER_PACKET_DROP);
}

bool Ieee8021qChecker::matchesPacket(const Packet *packet) const
{
    const auto& header = packet->peekAtFront<Ieee8021qHeader>();
    auto vlanId = header->getVid();
    return vlanIdFilter.empty() || std::find(vlanIdFilter.begin(), vlanIdFilter.end(), vlanId) != vlanIdFilter.end();
}

} // namespace inet

