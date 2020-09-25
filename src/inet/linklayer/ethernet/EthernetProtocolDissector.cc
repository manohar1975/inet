//
// Copyright (C) 2018 OpenSim Ltd.
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

#include "inet/common/packet/dissector/ProtocolDissectorRegistry.h"
#include "inet/common/ProtocolGroup.h"
#include "inet/common/ProtocolTag_m.h"
#include "inet/linklayer/common/EtherType_m.h"
#include "inet/linklayer/ethernet/Ethernet.h"
#include "inet/linklayer/ethernet/EthernetMacHeader_m.h"
#include "inet/linklayer/ethernet/EthernetProtocolDissector.h"
#include "inet/physicallayer/ethernet/EthernetPhyHeader_m.h"
#include "inet/protocol/fragmentation/tag/FragmentTag_m.h"

namespace inet {

using namespace inet::physicallayer;

Register_Protocol_Dissector(&Protocol::ethernetMac, EthernetMacDissector);
Register_Protocol_Dissector(&Protocol::ethernetPhy, EthernetPhyDissector);

void EthernetPhyDissector::dissect(Packet *packet, const Protocol *protocol, ICallback& callback) const
{
    const auto& header = packet->popAtFront<EthernetPhyHeaderBase>();
    callback.startProtocolDataUnit(&Protocol::ethernetPhy);
    callback.visitChunk(header, &Protocol::ethernetPhy);
    if (auto phyHeader = dynamicPtrCast<const EthernetFragmentPhyHeader>(header)) {
        const auto& fragmentTag = packet->findTag<FragmentTag>();
        if (phyHeader->getPreambleType() == SMD_Sx && fragmentTag != nullptr && fragmentTag->getLastFragment())
            callback.dissectPacket(packet, nullptr); // TODO: the Ethernet MAC protocol could be dissected here (but the packet cannot be dissected deeper, so providing that protocol is incorrect)
        else
            callback.dissectPacket(packet, nullptr);
    }
    else
        callback.dissectPacket(packet, &Protocol::ethernetMac);
    callback.endProtocolDataUnit(&Protocol::ethernetPhy);
}

void EthernetMacDissector::dissect(Packet *packet, const Protocol *protocol, ICallback& callback) const
{
    const auto& macAddressesHeader = packet->popAtFront<EthernetMacAddressFields>();
    callback.startProtocolDataUnit(&Protocol::ethernetMac);
    callback.visitChunk(macAddressesHeader, &Protocol::ethernetMac);
    const auto& fcs = packet->popAtBack<EthernetFcs>(ETHER_FCS_BYTES);
    int typeOrLength = -1;
    if (auto macHeader = dynamicPtrCast<const EthernetMacHeader>(macAddressesHeader))
        typeOrLength = macHeader->getTypeOrLength();
    else {
        while (typeOrLength == -1 || typeOrLength == 0x8100 || typeOrLength == 0x88A8) {
            const auto& typeOrLengthHeader = packet->popAtFront<EthernetTypeOrLengthField>();
            typeOrLength = typeOrLengthHeader->getTypeOrLength();
            callback.visitChunk(typeOrLengthHeader, &Protocol::ethernetMac);
        }
    }
    if (isEth2Type(typeOrLength)) {
        auto payloadProtocol = ProtocolGroup::ethertype.findProtocol(typeOrLength);
        callback.dissectPacket(packet, payloadProtocol);
    }
    else {
        // LLC header
        auto ethEndOffset = packet->getFrontOffset() + B(typeOrLength);
        auto trailerOffset = packet->getBackOffset();
        packet->setBackOffset(ethEndOffset);
        callback.dissectPacket(packet, &Protocol::ieee8022llc);
        packet->setBackOffset(trailerOffset);
    }
    auto paddingLength = packet->getDataLength();
    if (paddingLength > b(0)) {
        const auto& padding = packet->popAtFront(paddingLength);        // remove padding (type is not EthernetPadding!)
        callback.visitChunk(padding, &Protocol::ethernetMac);
    }
    callback.visitChunk(fcs, &Protocol::ethernetMac);
    callback.endProtocolDataUnit(&Protocol::ethernetMac);
}

} // namespace inet

