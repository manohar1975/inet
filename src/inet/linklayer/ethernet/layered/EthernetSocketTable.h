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

#ifndef __INET_ETHERNETSOCKETTABLE_H
#define __INET_ETHERNETSOCKETTABLE_H

#include "inet/common/INETDefs.h"
#include "inet/common/packet/Message.h"
#include "inet/common/packet/Packet.h"
#include "inet/linklayer/ethernet/EtherFrame_m.h"
#include "inet/linklayer/ethernet/EthernetCommand_m.h"

namespace inet {

class INET_API EthernetSocketTable : public cSimpleModule
{
  public:
    struct Socket
    {
        int socketId = -1;
        MacAddress localAddress;
        MacAddress remoteAddress;
        const Protocol *protocol = nullptr;
        int vlanId = -1;

        Socket(int socketId) : socketId(socketId) {}
        bool matches(Packet *packet, const Ptr<const EthernetMacHeader>& ethernetMacHeader);

        friend std::ostream& operator << (std::ostream& o, const Socket& t);
    };

  protected:
    std::map<int, Socket *> socketIdToSocketMap;

  protected:
    virtual void initialize() override;

  public:
    bool createSocket(int socketId, MacAddress loaclAddress, MacAddress remoteAddress, const Protocol *protocol, int vlanId);
    bool deleteSocket(int socketId);
    std::vector<Socket*> findSocketsFor(const Packet *packet) const;
};

} // namespace inet

#endif
