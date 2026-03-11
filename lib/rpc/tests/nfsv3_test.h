/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
Frame 3687: 112 bytes on wire (896 bits), 112 bytes captured (896 bits) on interface any, id 0
    Section number: 1
    Interface id: 0 (any)
        Interface name: any
    Encapsulation type: Linux cooked-mode capture v1 (25)
    Arrival Time: Apr  9, 2025 10:57:37.639355308 PDT
    UTC Arrival Time: Apr  9, 2025 17:57:37.639355308 UTC
    Epoch Arrival Time: 1744221457.639355308
    [Time shift for this packet: 0.000000000 seconds]
    [Time delta from previous captured frame: 0.000038153 seconds]
    [Time delta from previous displayed frame: 0.000000000 seconds]
    [Time since reference or first frame: 28.404337042 seconds]
    Frame Number: 3687
    Frame Length: 112 bytes (896 bits)
    Capture Length: 112 bytes (896 bits)
    [Frame is marked: False]
    [Frame is ignored: False]
    [Protocols in frame: sll:ethertype:ip:tcp:rpc]
Linux cooked capture v1
    Packet type: Sent by us (4)
    Link-layer address type: Ethernet (1)
    Link-layer address length: 6
    Source: IP3CenturyIn_3e:f7:8b (84:47:09:3e:f7:8b)
    Unused: d542
    Protocol: IPv4 (0x0800)
Internet Protocol Version 4, Src: 192.168.2.126, Dst: 192.168.2.127
    0100 .... = Version: 4
    .... 0101 = Header Length: 20 bytes (5)
    Differentiated Services Field: 0x00 (DSCP: CS0, ECN: Not-ECT)
        0000 00.. = Differentiated Services Codepoint: Default (0)
        .... ..00 = Explicit Congestion Notification: Not ECN-Capable Transport (0)
    Total Length: 96
    Identification: 0x2c06 (11270)
    010. .... = Flags: 0x2, Don't fragment
        0... .... = Reserved bit: Not set
        .1.. .... = Don't fragment: Set
        ..0. .... = More fragments: Not set
    ...0 0000 0000 0000 = Fragment Offset: 0
    Time to Live: 64
    Protocol: TCP (6)
    Header Checksum: 0x8844 [validation disabled]
    [Header checksum status: Unverified]
    Source Address: 192.168.2.126
    Destination Address: 192.168.2.127
    [Stream index: 1]
Transmission Control Protocol, Src Port: 54088, Dst Port: 2049, Seq: 1, Ack: 1, Len: 44
    Source Port: 54088
    Destination Port: 2049
    [Stream index: 9]
    [Stream Packet Number: 4]
    [Conversation completeness: Incomplete, ESTABLISHED (7)]
        ..0. .... = RST: Absent
        ...0 .... = FIN: Absent
        .... 0... = Data: Absent
        .... .1.. = ACK: Present
        .... ..1. = SYN-ACK: Present
        .... ...1 = SYN: Present
        [Completeness Flags: ···ASS]
    [TCP Segment Len: 44]
    Sequence Number: 1    (relative sequence number)
    Sequence Number (raw): 2808140083
    [Next Sequence Number: 45    (relative sequence number)]
    Acknowledgment Number: 1    (relative ack number)
    Acknowledgment number (raw): 3410093902
    1000 .... = Header Length: 32 bytes (8)
    Flags: 0x018 (PSH, ACK)
        000. .... .... = Reserved: Not set
        ...0 .... .... = Accurate ECN: Not set
        .... 0... .... = Congestion Window Reduced: Not set
        .... .0.. .... = ECN-Echo: Not set
        .... ..0. .... = Urgent: Not set
        .... ...1 .... = Acknowledgment: Set
        .... .... 1... = Push: Set
        .... .... .0.. = Reset: Not set
        .... .... ..0. = Syn: Not set
        .... .... ...0 = Fin: Not set
        [TCP Flags: ·······AP···]
    Window: 502
    [Calculated window size: 64256]
    [Window size scaling factor: 128]
    Checksum: 0x9134 [unverified]
    [Checksum Status: Unverified]
    Urgent Pointer: 0
    Options: (12 bytes), No-Operation (NOP), No-Operation (NOP), Timestamps
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - Timestamps: TSval 294755053, TSecr 320930801
            Kind: Time Stamp Option (8)
            Length: 10
            Timestamp value: 294755053
            Timestamp echo reply: 320930801
    [Timestamps]
        [Time since first frame in this TCP stream: 0.000191084 seconds]
        [Time since previous frame in this TCP stream: 0.000038153 seconds]
    [SEQ/ACK analysis]
        [iRTT: 0.000152931 seconds]
        [Bytes in flight: 44]
        [Bytes sent since last PSH flag: 44]
    TCP payload (44 bytes)
Remote Procedure Call, Type:Call XID:0x47f74448
    Fragment header: Last fragment, 40 bytes
        1... .... .... .... .... .... .... .... = Last Fragment: Yes
        .000 0000 0000 0000 0000 0000 0010 1000 = Fragment Length: 40
    XID: 0x47f74448 (1207387208)
    Message Type: Call (0)
    RPC Version: 2
    Program: NFS (100003)
    Program Version: 3
    Procedure: NULL (0)
    Credentials
        Flavor: AUTH_NULL (0)
        Length: 0
    Verifier
        Flavor: AUTH_NULL (0)
        Length: 0
Network File System
    [Program Version: 3]
    [V3 Procedure: NULL (0)]
*/

unsigned char nfs3_null_request_packet_data[] = {
	0x00, 0x04, 0x00, 0x01, 0x00, 0x06, 0x84, 0x47, 0x09, 0x3e, 0xf7, 0x8b,
	0xd5, 0x42, 0x08, 0x00, 0x45, 0x00, 0x00, 0x60, 0x2c, 0x06, 0x40, 0x00,
	0x40, 0x06, 0x88, 0x44, 0xc0, 0xa8, 0x02, 0x7e, 0xc0, 0xa8, 0x02, 0x7f,
	0xd3, 0x48, 0x08, 0x01, 0xa7, 0x60, 0xd1, 0x33, 0xcb, 0x41, 0xe7, 0x4e,
	0x80, 0x18, 0x01, 0xf6, 0x91, 0x34, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a,
	0x11, 0x91, 0x9a, 0xed, 0x13, 0x21, 0x03, 0xf1, 0x80, 0x00, 0x00, 0x28,
	0x47, 0xf7, 0x44, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x01, 0x86, 0xa3, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00
};

/*
Frame 3689: 96 bytes on wire (768 bits), 96 bytes captured (768 bits) on interface any, id 0
    Section number: 1
    Interface id: 0 (any)
        Interface name: any
    Encapsulation type: Linux cooked-mode capture v1 (25)
    Arrival Time: Apr  9, 2025 10:57:37.639577191 PDT
    UTC Arrival Time: Apr  9, 2025 17:57:37.639577191 UTC
    Epoch Arrival Time: 1744221457.639577191
    [Time shift for this packet: 0.000000000 seconds]
    [Time delta from previous captured frame: 0.000129327 seconds]
    [Time delta from previous displayed frame: 0.000221883 seconds]
    [Time since reference or first frame: 28.404558925 seconds]
    Frame Number: 3689
    Frame Length: 96 bytes (768 bits)
    Capture Length: 96 bytes (768 bits)
    [Frame is marked: False]
    [Frame is ignored: False]
    [Protocols in frame: sll:ethertype:ip:tcp:rpc]
Linux cooked capture v1
    Packet type: Unicast to us (0)
    Link-layer address type: Ethernet (1)
    Link-layer address length: 6
    Source: AZWTechnolog_4:ff:61 (e8:ff:1e:d4:ff:61)
    Unused: e59d
    Protocol: IPv4 (0x0800)
Internet Protocol Version 4, Src: 192.168.2.127, Dst: 192.168.2.126
    0100 .... = Version: 4
    .... 0101 = Header Length: 20 bytes (5)
    Differentiated Services Field: 0x00 (DSCP: CS0, ECN: Not-ECT)
        0000 00.. = Differentiated Services Codepoint: Default (0)
        .... ..00 = Explicit Congestion Notification: Not ECN-Capable Transport (0)
    Total Length: 80
    Identification: 0xcc85 (52357)
    010. .... = Flags: 0x2, Don't fragment
        0... .... = Reserved bit: Not set
        .1.. .... = Don't fragment: Set
        ..0. .... = More fragments: Not set
    ...0 0000 0000 0000 = Fragment Offset: 0
    Time to Live: 64
    Protocol: TCP (6)
    Header Checksum: 0xe7d4 [validation disabled]
    [Header checksum status: Unverified]
    Source Address: 192.168.2.127
    Destination Address: 192.168.2.126
    [Stream index: 1]
Transmission Control Protocol, Src Port: 2049, Dst Port: 54088, Seq: 1, Ack: 45, Len: 28
    Source Port: 2049
    Destination Port: 54088
    [Stream index: 9]
    [Stream Packet Number: 6]
    [Conversation completeness: Incomplete, DATA (15)]
        ..0. .... = RST: Absent
        ...0 .... = FIN: Absent
        .... 1... = Data: Present
        .... .1.. = ACK: Present
        .... ..1. = SYN-ACK: Present
        .... ...1 = SYN: Present
        [Completeness Flags: ··DASS]
    [TCP Segment Len: 28]
    Sequence Number: 1    (relative sequence number)
    Sequence Number (raw): 3410093902
    [Next Sequence Number: 29    (relative sequence number)]
    Acknowledgment Number: 45    (relative ack number)
    Acknowledgment number (raw): 2808140127
    1000 .... = Header Length: 32 bytes (8)
    Flags: 0x018 (PSH, ACK)
        000. .... .... = Reserved: Not set
        ...0 .... .... = Accurate ECN: Not set
        .... 0... .... = Congestion Window Reduced: Not set
        .... .0.. .... = ECN-Echo: Not set
        .... ..0. .... = Urgent: Not set
        .... ...1 .... = Acknowledgment: Set
        .... .... 1... = Push: Set
        .... .... .0.. = Reset: Not set
        .... .... ..0. = Syn: Not set
        .... .... ...0 = Fin: Not set
        [TCP Flags: ·······AP···]
    Window: 509
    [Calculated window size: 65152]
    [Window size scaling factor: 128]
    Checksum: 0x17ca [unverified]
    [Checksum Status: Unverified]
    Urgent Pointer: 0
    Options: (12 bytes), No-Operation (NOP), No-Operation (NOP), Timestamps
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - Timestamps: TSval 320930801, TSecr 294755053
            Kind: Time Stamp Option (8)
            Length: 10
            Timestamp value: 320930801
            Timestamp echo reply: 294755053
    [Timestamps]
        [Time since first frame in this TCP stream: 0.000412967 seconds]
        [Time since previous frame in this TCP stream: 0.000129327 seconds]
    [SEQ/ACK analysis]
        [iRTT: 0.000152931 seconds]
        [Bytes in flight: 28]
        [Bytes sent since last PSH flag: 28]
    TCP payload (28 bytes)
Remote Procedure Call, Type:Reply XID:0x47f74448
    Fragment header: Last fragment, 24 bytes
        1... .... .... .... .... .... .... .... = Last Fragment: Yes
        .000 0000 0000 0000 0000 0000 0001 1000 = Fragment Length: 24
    XID: 0x47f74448 (1207387208)
    Message Type: Reply (1)
    [Program: NFS (100003)]
    [Program Version: 3]
    [Procedure: NULL (0)]
    Reply State: accepted (0)
    [This is a reply to a request in frame 3687]
    [Time from request: 0.000221883 seconds]
    Verifier
        Flavor: AUTH_NULL (0)
        Length: 0
    Accept State: RPC executed successfully (0)
Network File System
    [Program Version: 3]
    [V3 Procedure: NULL (0)]
*/

unsigned char nfs3_null_reply_packet_data[] = {
	0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0xe8, 0xff, 0x1e, 0xd4, 0xff, 0x61,
	0xe5, 0x9d, 0x08, 0x00, 0x45, 0x00, 0x00, 0x50, 0xcc, 0x85, 0x40, 0x00,
	0x40, 0x06, 0xe7, 0xd4, 0xc0, 0xa8, 0x02, 0x7f, 0xc0, 0xa8, 0x02, 0x7e,
	0x08, 0x01, 0xd3, 0x48, 0xcb, 0x41, 0xe7, 0x4e, 0xa7, 0x60, 0xd1, 0x5f,
	0x80, 0x18, 0x01, 0xfd, 0x17, 0xca, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a,
	0x13, 0x21, 0x03, 0xf1, 0x11, 0x91, 0x9a, 0xed, 0x80, 0x00, 0x00, 0x18,
	0x47, 0xf7, 0x44, 0x48, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/*
Frame 3724: 156 bytes on wire (1248 bits), 156 bytes captured (1248 bits) on interface any, id 0
    Section number: 1
    Interface id: 0 (any)
        Interface name: any
    Encapsulation type: Linux cooked-mode capture v1 (25)
    Arrival Time: Apr  9, 2025 10:57:37.651000395 PDT
    UTC Arrival Time: Apr  9, 2025 17:57:37.651000395 UTC
    Epoch Arrival Time: 1744221457.651000395
    [Time shift for this packet: 0.000000000 seconds]
    [Time delta from previous captured frame: 0.000178510 seconds]
    [Time delta from previous displayed frame: 0.000524530 seconds]
    [Time since reference or first frame: 28.415982129 seconds]
    Frame Number: 3724
    Frame Length: 156 bytes (1248 bits)
    Capture Length: 156 bytes (1248 bits)
    [Frame is marked: False]
    [Frame is ignored: False]
    [Protocols in frame: sll:ethertype:ip:tcp:rpc:nfs]
Linux cooked capture v1
    Packet type: Sent by us (4)
    Link-layer address type: Ethernet (1)
    Link-layer address length: 6
    Source: IP3CenturyIn_3e:f7:8b (84:47:09:3e:f7:8b)
    Unused: 080a
    Protocol: IPv4 (0x0800)
Internet Protocol Version 4, Src: 192.168.2.126, Dst: 192.168.2.127
    0100 .... = Version: 4
    .... 0101 = Header Length: 20 bytes (5)
    Differentiated Services Field: 0x00 (DSCP: CS0, ECN: Not-ECT)
        0000 00.. = Differentiated Services Codepoint: Default (0)
        .... ..00 = Explicit Congestion Notification: Not ECN-Capable Transport (0)
    Total Length: 140
    Identification: 0xbc4d (48205)
    010. .... = Flags: 0x2, Don't fragment
        0... .... = Reserved bit: Not set
        .1.. .... = Don't fragment: Set
        ..0. .... = More fragments: Not set
    ...0 0000 0000 0000 = Fragment Offset: 0
    Time to Live: 64
    Protocol: TCP (6)
    Header Checksum: 0xf7d0 [validation disabled]
    [Header checksum status: Unverified]
    Source Address: 192.168.2.126
    Destination Address: 192.168.2.127
    [Stream index: 1]
Transmission Control Protocol, Src Port: 998, Dst Port: 2049, Seq: 181, Ack: 85, Len: 88
    Source Port: 998
    Destination Port: 2049
    [Stream index: 11]
    [Stream Packet Number: 12]
    [Conversation completeness: Incomplete, DATA (15)]
        ..0. .... = RST: Absent
        ...0 .... = FIN: Absent
        .... 1... = Data: Present
        .... .1.. = ACK: Present
        .... ..1. = SYN-ACK: Present
        .... ...1 = SYN: Present
        [Completeness Flags: ··DASS]
    [TCP Segment Len: 88]
    Sequence Number: 181    (relative sequence number)
    Sequence Number (raw): 1593290146
    [Next Sequence Number: 269    (relative sequence number)]
    Acknowledgment Number: 85    (relative ack number)
    Acknowledgment number (raw): 3027815794
    1000 .... = Header Length: 32 bytes (8)
    Flags: 0x018 (PSH, ACK)
        000. .... .... = Reserved: Not set
        ...0 .... .... = Accurate ECN: Not set
        .... 0... .... = Congestion Window Reduced: Not set
        .... .0.. .... = ECN-Echo: Not set
        .... ..0. .... = Urgent: Not set
        .... ...1 .... = Acknowledgment: Set
        .... .... 1... = Push: Set
        .... .... .0.. = Reset: Not set
        .... .... ..0. = Syn: Not set
        .... .... ...0 = Fin: Not set
        [TCP Flags: ·······AP···]
    Window: 502
    [Calculated window size: 64256]
    [Window size scaling factor: 128]
    Checksum: 0x0c8c [unverified]
    [Checksum Status: Unverified]
    Urgent Pointer: 0
    Options: (12 bytes), No-Operation (NOP), No-Operation (NOP), Timestamps
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - Timestamps: TSval 294755065, TSecr 320930812
            Kind: Time Stamp Option (8)
            Length: 10
            Timestamp value: 294755065
            Timestamp echo reply: 320930812
    [Timestamps]
        [Time since first frame in this TCP stream: 0.000897601 seconds]
        [Time since previous frame in this TCP stream: 0.000178510 seconds]
    [SEQ/ACK analysis]
        [This is an ACK to the segment in frame: 3723]
        [The RTT to ACK the segment was: 0.000178510 seconds]
        [iRTT: 0.000127844 seconds]
        [Bytes in flight: 88]
        [Bytes sent since last PSH flag: 88]
    TCP payload (88 bytes)
Remote Procedure Call, Type:Call XID:0xad46842e
    Fragment header: Last fragment, 84 bytes
        1... .... .... .... .... .... .... .... = Last Fragment: Yes
        .000 0000 0000 0000 0000 0000 0101 0100 = Fragment Length: 84
    XID: 0xad46842e (2907079726)
    Message Type: Call (0)
    RPC Version: 2
    Program: NFS (100003)
    Program Version: 3
    Procedure: FSINFO (19)
    Credentials
        Flavor: AUTH_UNIX (1)
        Length: 32
        Stamp: 0x00000000
        Machine Name: garbo
            length: 5
            contents: garbo
            fill bytes: opaque data
        UID: 0
        GID: 0
        Auxiliary GIDs (1) [0]
            GID: 0
    Verifier
        Flavor: AUTH_NULL (0)
        Length: 0
Network File System, FSINFO Call DH: 0x62d40c52
    [Program Version: 3]
    [V3 Procedure: FSINFO (19)]
    object
        length: 8
        [hash (CRC-32): 0x62d40c52]
        FileHandle: 0100010000000000
*/

unsigned char nfs3_fsinfo_request_packet_data[] = {
	0x00, 0x04, 0x00, 0x01, 0x00, 0x06, 0x84, 0x47, 0x09, 0x3e, 0xf7, 0x8b,
	0x08, 0x0a, 0x08, 0x00, 0x45, 0x00, 0x00, 0x8c, 0xbc, 0x4d, 0x40, 0x00,
	0x40, 0x06, 0xf7, 0xd0, 0xc0, 0xa8, 0x02, 0x7e, 0xc0, 0xa8, 0x02, 0x7f,
	0x03, 0xe6, 0x08, 0x01, 0x5e, 0xf7, 0xad, 0xa2, 0xb4, 0x78, 0xcd, 0x72,
	0x80, 0x18, 0x01, 0xf6, 0x0c, 0x8c, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a,
	0x11, 0x91, 0x9a, 0xf9, 0x13, 0x21, 0x03, 0xfc, 0x80, 0x00, 0x00, 0x54,
	0xad, 0x46, 0x84, 0x2e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x01, 0x86, 0xa3, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x13,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x05, 0x67, 0x61, 0x72, 0x62, 0x6f, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
};

/*
Frame 3725: 152 bytes on wire (1216 bits), 152 bytes captured (1216 bits) on interface any, id 0
    Section number: 1
    Interface id: 0 (any)
        Interface name: any
    Encapsulation type: Linux cooked-mode capture v1 (25)
    Arrival Time: Apr  9, 2025 10:57:37.652050998 PDT
    UTC Arrival Time: Apr  9, 2025 17:57:37.652050998 UTC
    Epoch Arrival Time: 1744221457.652050998
    [Time shift for this packet: 0.000000000 seconds]
    [Time delta from previous captured frame: 0.001050603 seconds]
    [Time delta from previous displayed frame: 0.001050603 seconds]
    [Time since reference or first frame: 28.417032732 seconds]
    Frame Number: 3725
    Frame Length: 152 bytes (1216 bits)
    Capture Length: 152 bytes (1216 bits)
    [Frame is marked: False]
    [Frame is ignored: False]
    [Protocols in frame: sll:ethertype:ip:tcp:rpc:nfs]
Linux cooked capture v1
    Packet type: Unicast to us (0)
    Link-layer address type: Ethernet (1)
    Link-layer address length: 6
    Source: AZWTechnolog_4:ff:61 (e8:ff:1e:d4:ff:61)
    Unused: 0000
    Protocol: IPv4 (0x0800)
Internet Protocol Version 4, Src: 192.168.2.127, Dst: 192.168.2.126
    0100 .... = Version: 4
    .... 0101 = Header Length: 20 bytes (5)
    Differentiated Services Field: 0x00 (DSCP: CS0, ECN: Not-ECT)
        0000 00.. = Differentiated Services Codepoint: Default (0)
        .... ..00 = Explicit Congestion Notification: Not ECN-Capable Transport (0)
    Total Length: 136
    Identification: 0x1f69 (8041)
    010. .... = Flags: 0x2, Don't fragment
        0... .... = Reserved bit: Not set
        .1.. .... = Don't fragment: Set
        ..0. .... = More fragments: Not set
    ...0 0000 0000 0000 = Fragment Offset: 0
    Time to Live: 64
    Protocol: TCP (6)
    Header Checksum: 0x94b9 [validation disabled]
    [Header checksum status: Unverified]
    Source Address: 192.168.2.127
    Destination Address: 192.168.2.126
    [Stream index: 1]
Transmission Control Protocol, Src Port: 2049, Dst Port: 998, Seq: 85, Ack: 269, Len: 84
    Source Port: 2049
    Destination Port: 998
    [Stream index: 11]
    [Stream Packet Number: 13]
    [Conversation completeness: Incomplete, DATA (15)]
        ..0. .... = RST: Absent
        ...0 .... = FIN: Absent
        .... 1... = Data: Present
        .... .1.. = ACK: Present
        .... ..1. = SYN-ACK: Present
        .... ...1 = SYN: Present
        [Completeness Flags: ··DASS]
    [TCP Segment Len: 84]
    Sequence Number: 85    (relative sequence number)
    Sequence Number (raw): 3027815794
    [Next Sequence Number: 169    (relative sequence number)]
    Acknowledgment Number: 269    (relative ack number)
    Acknowledgment number (raw): 1593290234
    1000 .... = Header Length: 32 bytes (8)
    Flags: 0x018 (PSH, ACK)
        000. .... .... = Reserved: Not set
        ...0 .... .... = Accurate ECN: Not set
        .... 0... .... = Congestion Window Reduced: Not set
        .... .0.. .... = ECN-Echo: Not set
        .... ..0. .... = Urgent: Not set
        .... ...1 .... = Acknowledgment: Set
        .... .... 1... = Push: Set
        .... .... .0.. = Reset: Not set
        .... .... ..0. = Syn: Not set
        .... .... ...0 = Fin: Not set
        [TCP Flags: ·······AP···]
    Window: 509
    [Calculated window size: 65152]
    [Window size scaling factor: 128]
    Checksum: 0x3d77 [unverified]
    [Checksum Status: Unverified]
    Urgent Pointer: 0
    Options: (12 bytes), No-Operation (NOP), No-Operation (NOP), Timestamps
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - Timestamps: TSval 320930813, TSecr 294755065
            Kind: Time Stamp Option (8)
            Length: 10
            Timestamp value: 320930813
            Timestamp echo reply: 294755065
    [Timestamps]
        [Time since first frame in this TCP stream: 0.001948204 seconds]
        [Time since previous frame in this TCP stream: 0.001050603 seconds]
    [SEQ/ACK analysis]
        [This is an ACK to the segment in frame: 3724]
        [The RTT to ACK the segment was: 0.001050603 seconds]
        [iRTT: 0.000127844 seconds]
        [Bytes in flight: 84]
        [Bytes sent since last PSH flag: 84]
    TCP payload (84 bytes)
Remote Procedure Call, Type:Reply XID:0xad46842e
    Fragment header: Last fragment, 80 bytes
        1... .... .... .... .... .... .... .... = Last Fragment: Yes
        .000 0000 0000 0000 0000 0000 0101 0000 = Fragment Length: 80
    XID: 0xad46842e (2907079726)
    Message Type: Reply (1)
    [Program: NFS (100003)]
    [Program Version: 3]
    [Procedure: FSINFO (19)]
    Reply State: accepted (0)
    [This is a reply to a request in frame 3724]
    [Time from request: 0.001050603 seconds]
    Verifier
        Flavor: AUTH_NULL (0)
        Length: 0
    Accept State: RPC executed successfully (0)
Network File System, FSINFO Reply
    [Program Version: 3]
    [V3 Procedure: FSINFO (19)]
    Status: NFS3_OK (0)
    obj_attributes
        attributes_follow: no value (0)
    rtmax: 1048576
    rtpref: 1048576
    rtmult: 4096
    wtmax: 1048576
    wtpref: 1048576
    wtmult: 4096
    dtpref: 1048576
    maxfilesize: 9223372036854775807
    time delta: 1.000000000 seconds
        seconds: 1
        nano seconds: 0
    Properties: 0x0000001b, SETATTR can set time on server, PATHCONF, File System supports symbolic links, File System supports hard links
        .... .... .... .... .... .... ...1 .... = SETATTR can set time on server: Yes
        .... .... .... .... .... .... .... 1... = PATHCONF: is valid for all files
        .... .... .... .... .... .... .... ..1. = File System supports symbolic links: Yes
        .... .... .... .... .... .... .... ...1 = File System supports hard links: Yes
*/

unsigned char nfs3_fsinfo_reply_packet_data[] = {
	0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0xe8, 0xff, 0x1e, 0xd4, 0xff, 0x61,
	0x00, 0x00, 0x08, 0x00, 0x45, 0x00, 0x00, 0x88, 0x1f, 0x69, 0x40, 0x00,
	0x40, 0x06, 0x94, 0xb9, 0xc0, 0xa8, 0x02, 0x7f, 0xc0, 0xa8, 0x02, 0x7e,
	0x08, 0x01, 0x03, 0xe6, 0xb4, 0x78, 0xcd, 0x72, 0x5e, 0xf7, 0xad, 0xfa,
	0x80, 0x18, 0x01, 0xfd, 0x3d, 0x77, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a,
	0x13, 0x21, 0x03, 0xfd, 0x11, 0x91, 0x9a, 0xf9, 0x80, 0x00, 0x00, 0x50,
	0xad, 0x46, 0x84, 0x2e, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
	0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00, 0x00,
	0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x10, 0x00, 0x00,
	0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1b
};

/*
Frame 3728: 156 bytes on wire (1248 bits), 156 bytes captured (1248 bits) on interface any, id 0
    Section number: 1
    Interface id: 0 (any)
        Interface name: any
    Encapsulation type: Linux cooked-mode capture v1 (25)
    Arrival Time: Apr  9, 2025 10:57:37.652286226 PDT
    UTC Arrival Time: Apr  9, 2025 17:57:37.652286226 UTC
    Epoch Arrival Time: 1744221457.652286226
    [Time shift for this packet: 0.000000000 seconds]
    [Time delta from previous captured frame: 0.000028674 seconds]
    [Time delta from previous displayed frame: 0.000028674 seconds]
    [Time since reference or first frame: 28.417267960 seconds]
    Frame Number: 3728
    Frame Length: 156 bytes (1248 bits)
    Capture Length: 156 bytes (1248 bits)
    [Frame is marked: False]
    [Frame is ignored: False]
    [Protocols in frame: sll:ethertype:ip:tcp:rpc:nfs]
Linux cooked capture v1
    Packet type: Sent by us (4)
    Link-layer address type: Ethernet (1)
    Link-layer address length: 6
    Source: IP3CenturyIn_3e:f7:8b (84:47:09:3e:f7:8b)
    Unused: 090e
    Protocol: IPv4 (0x0800)
Internet Protocol Version 4, Src: 192.168.2.126, Dst: 192.168.2.127
    0100 .... = Version: 4
    .... 0101 = Header Length: 20 bytes (5)
    Differentiated Services Field: 0x00 (DSCP: CS0, ECN: Not-ECT)
        0000 00.. = Differentiated Services Codepoint: Default (0)
        .... ..00 = Explicit Congestion Notification: Not ECN-Capable Transport (0)
    Total Length: 140
    Identification: 0xbc4f (48207)
    010. .... = Flags: 0x2, Don't fragment
        0... .... = Reserved bit: Not set
        .1.. .... = Don't fragment: Set
        ..0. .... = More fragments: Not set
    ...0 0000 0000 0000 = Fragment Offset: 0
    Time to Live: 64
    Protocol: TCP (6)
    Header Checksum: 0xf7ce [validation disabled]
    [Header checksum status: Unverified]
    Source Address: 192.168.2.126
    Destination Address: 192.168.2.127
    [Stream index: 1]
Transmission Control Protocol, Src Port: 998, Dst Port: 2049, Seq: 357, Ack: 229, Len: 88
    Source Port: 998
    Destination Port: 2049
    [Stream index: 11]
    [Stream Packet Number: 16]
    [Conversation completeness: Incomplete, DATA (15)]
        ..0. .... = RST: Absent
        ...0 .... = FIN: Absent
        .... 1... = Data: Present
        .... .1.. = ACK: Present
        .... ..1. = SYN-ACK: Present
        .... ...1 = SYN: Present
        [Completeness Flags: ··DASS]
    [TCP Segment Len: 88]
    Sequence Number: 357    (relative sequence number)
    Sequence Number (raw): 1593290322
    [Next Sequence Number: 445    (relative sequence number)]
    Acknowledgment Number: 229    (relative ack number)
    Acknowledgment number (raw): 3027815938
    1000 .... = Header Length: 32 bytes (8)
    Flags: 0x018 (PSH, ACK)
        000. .... .... = Reserved: Not set
        ...0 .... .... = Accurate ECN: Not set
        .... 0... .... = Congestion Window Reduced: Not set
        .... .0.. .... = ECN-Echo: Not set
        .... ..0. .... = Urgent: Not set
        .... ...1 .... = Acknowledgment: Set
        .... .... 1... = Push: Set
        .... .... .0.. = Reset: Not set
        .... .... ..0. = Syn: Not set
        .... .... ...0 = Fin: Not set
        [TCP Flags: ·······AP···]
    Window: 502
    [Calculated window size: 64256]
    [Window size scaling factor: 128]
    Checksum: 0x095b [unverified]
    [Checksum Status: Unverified]
    Urgent Pointer: 0
    Options: (12 bytes), No-Operation (NOP), No-Operation (NOP), Timestamps
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - Timestamps: TSval 294755066, TSecr 320930814
            Kind: Time Stamp Option (8)
            Length: 10
            Timestamp value: 294755066
            Timestamp echo reply: 320930814
    [Timestamps]
        [Time since first frame in this TCP stream: 0.002183432 seconds]
        [Time since previous frame in this TCP stream: 0.000028674 seconds]
    [SEQ/ACK analysis]
        [This is an ACK to the segment in frame: 3727]
        [The RTT to ACK the segment was: 0.000028674 seconds]
        [iRTT: 0.000127844 seconds]
        [Bytes in flight: 88]
        [Bytes sent since last PSH flag: 88]
    TCP payload (88 bytes)
Remote Procedure Call, Type:Call XID:0xaf46842e
    Fragment header: Last fragment, 84 bytes
        1... .... .... .... .... .... .... .... = Last Fragment: Yes
        .000 0000 0000 0000 0000 0000 0101 0100 = Fragment Length: 84
    XID: 0xaf46842e (2940634158)
    Message Type: Call (0)
    RPC Version: 2
    Program: NFS (100003)
    Program Version: 3
    Procedure: GETATTR (1)
    Credentials
        Flavor: AUTH_UNIX (1)
        Length: 32
        Stamp: 0x00000000
        Machine Name: garbo
            length: 5
            contents: garbo
            fill bytes: opaque data
        UID: 0
        GID: 0
        Auxiliary GIDs (1) [0]
            GID: 0
    Verifier
        Flavor: AUTH_NULL (0)
        Length: 0
Network File System, GETATTR Call FH: 0x62d40c52
    [Program Version: 3]
    [V3 Procedure: GETATTR (1)]
    object
        length: 8
        [hash (CRC-32): 0x62d40c52]
        FileHandle: 0100010000000000
*/

unsigned char nfs3_getattr_root_request_packet_data[] = {
	0x00, 0x04, 0x00, 0x01, 0x00, 0x06, 0x84, 0x47, 0x09, 0x3e, 0xf7, 0x8b,
	0x09, 0x0e, 0x08, 0x00, 0x45, 0x00, 0x00, 0x8c, 0xbc, 0x4f, 0x40, 0x00,
	0x40, 0x06, 0xf7, 0xce, 0xc0, 0xa8, 0x02, 0x7e, 0xc0, 0xa8, 0x02, 0x7f,
	0x03, 0xe6, 0x08, 0x01, 0x5e, 0xf7, 0xae, 0x52, 0xb4, 0x78, 0xce, 0x02,
	0x80, 0x18, 0x01, 0xf6, 0x09, 0x5b, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a,
	0x11, 0x91, 0x9a, 0xfa, 0x13, 0x21, 0x03, 0xfe, 0x80, 0x00, 0x00, 0x54,
	0xaf, 0x46, 0x84, 0x2e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x01, 0x86, 0xa3, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x05, 0x67, 0x61, 0x72, 0x62, 0x6f, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
};

/*
Frame 3729: 184 bytes on wire (1472 bits), 184 bytes captured (1472 bits) on interface any, id 0
    Section number: 1
    Interface id: 0 (any)
        Interface name: any
    Encapsulation type: Linux cooked-mode capture v1 (25)
    Arrival Time: Apr  9, 2025 10:57:37.652438406 PDT
    UTC Arrival Time: Apr  9, 2025 17:57:37.652438406 UTC
    Epoch Arrival Time: 1744221457.652438406
    [Time shift for this packet: 0.000000000 seconds]
    [Time delta from previous captured frame: 0.000152180 seconds]
    [Time delta from previous displayed frame: 0.000152180 seconds]
    [Time since reference or first frame: 28.417420140 seconds]
    Frame Number: 3729
    Frame Length: 184 bytes (1472 bits)
    Capture Length: 184 bytes (1472 bits)
    [Frame is marked: False]
    [Frame is ignored: False]
    [Protocols in frame: sll:ethertype:ip:tcp:rpc:nfs]
Linux cooked capture v1
    Packet type: Unicast to us (0)
    Link-layer address type: Ethernet (1)
    Link-layer address length: 6
    Source: AZWTechnolog_4:ff:61 (e8:ff:1e:d4:ff:61)
    Unused: 26d4
    Protocol: IPv4 (0x0800)
Internet Protocol Version 4, Src: 192.168.2.127, Dst: 192.168.2.126
    0100 .... = Version: 4
    .... 0101 = Header Length: 20 bytes (5)
    Differentiated Services Field: 0x00 (DSCP: CS0, ECN: Not-ECT)
        0000 00.. = Differentiated Services Codepoint: Default (0)
        .... ..00 = Explicit Congestion Notification: Not ECN-Capable Transport (0)
    Total Length: 168
    Identification: 0x1f6b (8043)
    010. .... = Flags: 0x2, Don't fragment
        0... .... = Reserved bit: Not set
        .1.. .... = Don't fragment: Set
        ..0. .... = More fragments: Not set
    ...0 0000 0000 0000 = Fragment Offset: 0
    Time to Live: 64
    Protocol: TCP (6)
    Header Checksum: 0x9497 [validation disabled]
    [Header checksum status: Unverified]
    Source Address: 192.168.2.127
    Destination Address: 192.168.2.126
    [Stream index: 1]
Transmission Control Protocol, Src Port: 2049, Dst Port: 998, Seq: 229, Ack: 445, Len: 116
    Source Port: 2049
    Destination Port: 998
    [Stream index: 11]
    [Stream Packet Number: 17]
    [Conversation completeness: Incomplete, DATA (15)]
        ..0. .... = RST: Absent
        ...0 .... = FIN: Absent
        .... 1... = Data: Present
        .... .1.. = ACK: Present
        .... ..1. = SYN-ACK: Present
        .... ...1 = SYN: Present
        [Completeness Flags: ··DASS]
    [TCP Segment Len: 116]
    Sequence Number: 229    (relative sequence number)
    Sequence Number (raw): 3027815938
    [Next Sequence Number: 345    (relative sequence number)]
    Acknowledgment Number: 445    (relative ack number)
    Acknowledgment number (raw): 1593290410
    1000 .... = Header Length: 32 bytes (8)
    Flags: 0x018 (PSH, ACK)
        000. .... .... = Reserved: Not set
        ...0 .... .... = Accurate ECN: Not set
        .... 0... .... = Congestion Window Reduced: Not set
        .... .0.. .... = ECN-Echo: Not set
        .... ..0. .... = Urgent: Not set
        .... ...1 .... = Acknowledgment: Set
        .... .... 1... = Push: Set
        .... .... .0.. = Reset: Not set
        .... .... ..0. = Syn: Not set
        .... .... ...0 = Fin: Not set
        [TCP Flags: ·······AP···]
    Window: 509
    [Calculated window size: 65152]
    [Window size scaling factor: 128]
    Checksum: 0xd7c5 [unverified]
    [Checksum Status: Unverified]
    Urgent Pointer: 0
    Options: (12 bytes), No-Operation (NOP), No-Operation (NOP), Timestamps
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - Timestamps: TSval 320930814, TSecr 294755066
            Kind: Time Stamp Option (8)
            Length: 10
            Timestamp value: 320930814
            Timestamp echo reply: 294755066
    [Timestamps]
        [Time since first frame in this TCP stream: 0.002335612 seconds]
        [Time since previous frame in this TCP stream: 0.000152180 seconds]
    [SEQ/ACK analysis]
        [This is an ACK to the segment in frame: 3728]
        [The RTT to ACK the segment was: 0.000152180 seconds]
        [iRTT: 0.000127844 seconds]
        [Bytes in flight: 116]
        [Bytes sent since last PSH flag: 116]
    TCP payload (116 bytes)
Remote Procedure Call, Type:Reply XID:0xaf46842e
    Fragment header: Last fragment, 112 bytes
        1... .... .... .... .... .... .... .... = Last Fragment: Yes
        .000 0000 0000 0000 0000 0000 0111 0000 = Fragment Length: 112
    XID: 0xaf46842e (2940634158)
    Message Type: Reply (1)
    [Program: NFS (100003)]
    [Program Version: 3]
    [Procedure: GETATTR (1)]
    Reply State: accepted (0)
    [This is a reply to a request in frame 3728]
    [Time from request: 0.000152180 seconds]
    Verifier
        Flavor: AUTH_NULL (0)
        Length: 0
    Accept State: RPC executed successfully (0)
Network File System, GETATTR Reply  Directory mode: 0755 uid: 0 gid: 0
    [Program Version: 3]
    [V3 Procedure: GETATTR (1)]
    Status: NFS3_OK (0)
    obj_attributes  Directory mode: 0755 uid: 0 gid: 0
        Type: Directory (2)
        Mode: 0755, S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IXGRP, S_IROTH, S_IXOTH
            .... .... .... .... .... 0... .... .... = S_ISUID: No
            .... .... .... .... .... .0.. .... .... = S_ISGID: No
            .... .... .... .... .... ..0. .... .... = S_ISVTX: No
            .... .... .... .... .... ...1 .... .... = S_IRUSR: Yes
            .... .... .... .... .... .... 1... .... = S_IWUSR: Yes
            .... .... .... .... .... .... .1.. .... = S_IXUSR: Yes
            .... .... .... .... .... .... ..1. .... = S_IRGRP: Yes
            .... .... .... .... .... .... ...0 .... = S_IWGRP: No
            .... .... .... .... .... .... .... 1... = S_IXGRP: Yes
            .... .... .... .... .... .... .... .1.. = S_IROTH: Yes
            .... .... .... .... .... .... .... ..0. = S_IWOTH: No
            .... .... .... .... .... .... .... ...1 = S_IXOTH: Yes
        nlink: 2
        uid: 0
        gid: 0
        size: 6
        used: 0
        rdev: 0,0
            specdata1: 0
            specdata2: 0
        fsid: 0x0000000000000000 (0)
        fileid: 268662440
        atime: Apr  9, 2025 10:55:52.125736320 PDT
            seconds: 1744221352
            nano seconds: 125736320
        mtime: Jul 16, 2024 17:00:00.000000000 PDT
            seconds: 1721174400
            nano seconds: 0
        ctime: Mar 24, 2025 08:12:31.959723726 PDT
            seconds: 1742829151
            nano seconds: 959723726
*/

unsigned char nfs3_getattr_root_reply_packet_data[] = {
	0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0xe8, 0xff, 0x1e, 0xd4, 0xff, 0x61,
	0x26, 0xd4, 0x08, 0x00, 0x45, 0x00, 0x00, 0xa8, 0x1f, 0x6b, 0x40, 0x00,
	0x40, 0x06, 0x94, 0x97, 0xc0, 0xa8, 0x02, 0x7f, 0xc0, 0xa8, 0x02, 0x7e,
	0x08, 0x01, 0x03, 0xe6, 0xb4, 0x78, 0xce, 0x02, 0x5e, 0xf7, 0xae, 0xaa,
	0x80, 0x18, 0x01, 0xfd, 0xd7, 0xc5, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a,
	0x13, 0x21, 0x03, 0xfe, 0x11, 0x91, 0x9a, 0xfa, 0x80, 0x00, 0x00, 0x70,
	0xaf, 0x46, 0x84, 0x2e, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x01, 0xed,
	0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x10, 0x03, 0x76, 0xa8, 0x67, 0xf6, 0xb4, 0xa8, 0x07, 0x7e, 0x95, 0x80,
	0x66, 0x97, 0x09, 0x80, 0x00, 0x00, 0x00, 0x00, 0x67, 0xe1, 0x76, 0x5f,
	0x39, 0x34, 0x38, 0xce
};

/*
Frame 4339: 160 bytes on wire (1280 bits), 160 bytes captured (1280 bits) on interface any, id 0
    Section number: 1
    Interface id: 0 (any)
        Interface name: any
    Encapsulation type: Linux cooked-mode capture v1 (25)
    Arrival Time: Apr  9, 2025 10:57:41.670897084 PDT
    UTC Arrival Time: Apr  9, 2025 17:57:41.670897084 UTC
    Epoch Arrival Time: 1744221461.670897084
    [Time shift for this packet: 0.000000000 seconds]
    [Time delta from previous captured frame: 0.000182057 seconds]
    [Time delta from previous displayed frame: 0.000182057 seconds]
    [Time since reference or first frame: 32.435878818 seconds]
    Frame Number: 4339
    Frame Length: 160 bytes (1280 bits)
    Capture Length: 160 bytes (1280 bits)
    [Frame is marked: False]
    [Frame is ignored: False]
    [Protocols in frame: sll:ethertype:ip:tcp:rpc:nfs]
Linux cooked capture v1
    Packet type: Sent by us (4)
    Link-layer address type: Ethernet (1)
    Link-layer address length: 6
    Source: IP3CenturyIn_3e:f7:8b (84:47:09:3e:f7:8b)
    Unused: 0800
    Protocol: IPv4 (0x0800)
Internet Protocol Version 4, Src: 192.168.2.126, Dst: 192.168.2.127
    0100 .... = Version: 4
    .... 0101 = Header Length: 20 bytes (5)
    Differentiated Services Field: 0x00 (DSCP: CS0, ECN: Not-ECT)
        0000 00.. = Differentiated Services Codepoint: Default (0)
        .... ..00 = Explicit Congestion Notification: Not ECN-Capable Transport (0)
    Total Length: 144
    Identification: 0xbc57 (48215)
    010. .... = Flags: 0x2, Don't fragment
        0... .... = Reserved bit: Not set
        .1.. .... = Don't fragment: Set
        ..0. .... = More fragments: Not set
    ...0 0000 0000 0000 = Fragment Offset: 0
    Time to Live: 64
    Protocol: TCP (6)
    Header Checksum: 0xf7c2 [validation disabled]
    [Header checksum status: Unverified]
    Source Address: 192.168.2.126
    Destination Address: 192.168.2.127
    [Stream index: 1]
Transmission Control Protocol, Src Port: 998, Dst Port: 2049, Seq: 845, Ack: 877, Len: 92
    Source Port: 998
    Destination Port: 2049
    [Stream index: 11]
    [Stream Packet Number: 30]
    [Conversation completeness: Incomplete, DATA (15)]
        ..0. .... = RST: Absent
        ...0 .... = FIN: Absent
        .... 1... = Data: Present
        .... .1.. = ACK: Present
        .... ..1. = SYN-ACK: Present
        .... ...1 = SYN: Present
        [Completeness Flags: ··DASS]
    [TCP Segment Len: 92]
    Sequence Number: 845    (relative sequence number)
    Sequence Number (raw): 1593290810
    [Next Sequence Number: 937    (relative sequence number)]
    Acknowledgment Number: 877    (relative ack number)
    Acknowledgment number (raw): 3027816586
    1000 .... = Header Length: 32 bytes (8)
    Flags: 0x018 (PSH, ACK)
        000. .... .... = Reserved: Not set
        ...0 .... .... = Accurate ECN: Not set
        .... 0... .... = Congestion Window Reduced: Not set
        .... .0.. .... = ECN-Echo: Not set
        .... ..0. .... = Urgent: Not set
        .... ...1 .... = Acknowledgment: Set
        .... .... 1... = Push: Set
        .... .... .0.. = Reset: Not set
        .... .... ..0. = Syn: Not set
        .... .... ...0 = Fin: Not set
        [TCP Flags: ·······AP···]
    Window: 501
    [Calculated window size: 64128]
    [Window size scaling factor: 128]
    Checksum: 0xdb1e [unverified]
    [Checksum Status: Unverified]
    Urgent Pointer: 0
    Options: (12 bytes), No-Operation (NOP), No-Operation (NOP), Timestamps
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - Timestamps: TSval 294759085, TSecr 320934832
            Kind: Time Stamp Option (8)
            Length: 10
            Timestamp value: 294759085
            Timestamp echo reply: 320934832
    [Timestamps]
        [Time since first frame in this TCP stream: 4.020794290 seconds]
        [Time since previous frame in this TCP stream: 0.000182057 seconds]
    [SEQ/ACK analysis]
        [This is an ACK to the segment in frame: 4338]
        [The RTT to ACK the segment was: 0.000182057 seconds]
        [iRTT: 0.000127844 seconds]
        [Bytes in flight: 92]
        [Bytes sent since last PSH flag: 92]
    TCP payload (92 bytes)
Remote Procedure Call, Type:Call XID:0xb546842e
    Fragment header: Last fragment, 88 bytes
        1... .... .... .... .... .... .... .... = Last Fragment: Yes
        .000 0000 0000 0000 0000 0000 0101 1000 = Fragment Length: 88
    XID: 0xb546842e (3041297454)
    Message Type: Call (0)
    RPC Version: 2
    Program: NFS (100003)
    Program Version: 3
    Procedure: ACCESS (4)
    Credentials
        Flavor: AUTH_UNIX (1)
        Length: 32
        Stamp: 0x00000000
        Machine Name: garbo
            length: 5
            contents: garbo
            fill bytes: opaque data
        UID: 1066
        GID: 10
        Auxiliary GIDs (1) [10]
            GID: 10
    Verifier
        Flavor: AUTH_NULL (0)
        Length: 0
Network File System, ACCESS Call, FH: 0x62d40c52, [Check: RD LU MD XT DL]
    [Program Version: 3]
    [V3 Procedure: ACCESS (4)]
    object
        length: 8
        [hash (CRC-32): 0x62d40c52]
        FileHandle: 0100010000000000
    Check access: 0x0000001f
        .... ...1 = 0x001 READ: allowed?
        .... ..1. = 0x002 LOOKUP: allowed?
        .... .1.. = 0x004 MODIFY: allowed?
        .... 1... = 0x008 EXTEND: allowed?
        ...1 .... = 0x010 DELETE: allowed?
*/

unsigned char nfs3_access_root_request_packet_data[] = {
	0x00, 0x04, 0x00, 0x01, 0x00, 0x06, 0x84, 0x47, 0x09, 0x3e, 0xf7, 0x8b,
	0x08, 0x00, 0x08, 0x00, 0x45, 0x00, 0x00, 0x90, 0xbc, 0x57, 0x40, 0x00,
	0x40, 0x06, 0xf7, 0xc2, 0xc0, 0xa8, 0x02, 0x7e, 0xc0, 0xa8, 0x02, 0x7f,
	0x03, 0xe6, 0x08, 0x01, 0x5e, 0xf7, 0xb0, 0x3a, 0xb4, 0x78, 0xd0, 0x8a,
	0x80, 0x18, 0x01, 0xf5, 0xdb, 0x1e, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a,
	0x11, 0x91, 0xaa, 0xad, 0x13, 0x21, 0x13, 0xb0, 0x80, 0x00, 0x00, 0x58,
	0xb5, 0x46, 0x84, 0x2e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x01, 0x86, 0xa3, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x05, 0x67, 0x61, 0x72, 0x62, 0x6f, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x1f
};

/*
Frame 4340: 192 bytes on wire (1536 bits), 192 bytes captured (1536 bits) on interface any, id 0
    Section number: 1
    Interface id: 0 (any)
        Interface name: any
    Encapsulation type: Linux cooked-mode capture v1 (25)
    Arrival Time: Apr  9, 2025 10:57:41.671194832 PDT
    UTC Arrival Time: Apr  9, 2025 17:57:41.671194832 UTC
    Epoch Arrival Time: 1744221461.671194832
    [Time shift for this packet: 0.000000000 seconds]
    [Time delta from previous captured frame: 0.000297748 seconds]
    [Time delta from previous displayed frame: 0.000297748 seconds]
    [Time since reference or first frame: 32.436176566 seconds]
    Frame Number: 4340
    Frame Length: 192 bytes (1536 bits)
    Capture Length: 192 bytes (1536 bits)
    [Frame is marked: False]
    [Frame is ignored: False]
    [Protocols in frame: sll:ethertype:ip:tcp:rpc:nfs]
Linux cooked capture v1
    Packet type: Unicast to us (0)
    Link-layer address type: Ethernet (1)
    Link-layer address length: 6
    Source: AZWTechnolog_4:ff:61 (e8:ff:1e:d4:ff:61)
    Unused: 0800
    Protocol: IPv4 (0x0800)
Internet Protocol Version 4, Src: 192.168.2.127, Dst: 192.168.2.126
    0100 .... = Version: 4
    .... 0101 = Header Length: 20 bytes (5)
    Differentiated Services Field: 0x00 (DSCP: CS0, ECN: Not-ECT)
        0000 00.. = Differentiated Services Codepoint: Default (0)
        .... ..00 = Explicit Congestion Notification: Not ECN-Capable Transport (0)
    Total Length: 176
    Identification: 0x1f71 (8049)
    010. .... = Flags: 0x2, Don't fragment
        0... .... = Reserved bit: Not set
        .1.. .... = Don't fragment: Set
        ..0. .... = More fragments: Not set
    ...0 0000 0000 0000 = Fragment Offset: 0
    Time to Live: 64
    Protocol: TCP (6)
    Header Checksum: 0x9489 [validation disabled]
    [Header checksum status: Unverified]
    Source Address: 192.168.2.127
    Destination Address: 192.168.2.126
    [Stream index: 1]
Transmission Control Protocol, Src Port: 2049, Dst Port: 998, Seq: 877, Ack: 937, Len: 124
    Source Port: 2049
    Destination Port: 998
    [Stream index: 11]
    [Stream Packet Number: 31]
    [Conversation completeness: Incomplete, DATA (15)]
        ..0. .... = RST: Absent
        ...0 .... = FIN: Absent
        .... 1... = Data: Present
        .... .1.. = ACK: Present
        .... ..1. = SYN-ACK: Present
        .... ...1 = SYN: Present
        [Completeness Flags: ··DASS]
    [TCP Segment Len: 124]
    Sequence Number: 877    (relative sequence number)
    Sequence Number (raw): 3027816586
    [Next Sequence Number: 1001    (relative sequence number)]
    Acknowledgment Number: 937    (relative ack number)
    Acknowledgment number (raw): 1593290902
    1000 .... = Header Length: 32 bytes (8)
    Flags: 0x018 (PSH, ACK)
        000. .... .... = Reserved: Not set
        ...0 .... .... = Accurate ECN: Not set
        .... 0... .... = Congestion Window Reduced: Not set
        .... .0.. .... = ECN-Echo: Not set
        .... ..0. .... = Urgent: Not set
        .... ...1 .... = Acknowledgment: Set
        .... .... 1... = Push: Set
        .... .... .0.. = Reset: Not set
        .... .... ..0. = Syn: Not set
        .... .... ...0 = Fin: Not set
        [TCP Flags: ·······AP···]
    Window: 509
    [Calculated window size: 65152]
    [Window size scaling factor: 128]
    Checksum: 0xadd7 [unverified]
    [Checksum Status: Unverified]
    Urgent Pointer: 0
    Options: (12 bytes), No-Operation (NOP), No-Operation (NOP), Timestamps
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - Timestamps: TSval 320934833, TSecr 294759085
            Kind: Time Stamp Option (8)
            Length: 10
            Timestamp value: 320934833
            Timestamp echo reply: 294759085
    [Timestamps]
        [Time since first frame in this TCP stream: 4.021092038 seconds]
        [Time since previous frame in this TCP stream: 0.000297748 seconds]
    [SEQ/ACK analysis]
        [This is an ACK to the segment in frame: 4339]
        [The RTT to ACK the segment was: 0.000297748 seconds]
        [iRTT: 0.000127844 seconds]
        [Bytes in flight: 124]
        [Bytes sent since last PSH flag: 124]
    TCP payload (124 bytes)
Remote Procedure Call, Type:Reply XID:0xb546842e
    Fragment header: Last fragment, 120 bytes
        1... .... .... .... .... .... .... .... = Last Fragment: Yes
        .000 0000 0000 0000 0000 0000 0111 1000 = Fragment Length: 120
    XID: 0xb546842e (3041297454)
    Message Type: Reply (1)
    [Program: NFS (100003)]
    [Program Version: 3]
    [Procedure: ACCESS (4)]
    Reply State: accepted (0)
    [This is a reply to a request in frame 4339]
    [Time from request: 0.000297748 seconds]
    Verifier
        Flavor: AUTH_NULL (0)
        Length: 0
    Accept State: RPC executed successfully (0)
Network File System, ACCESS Reply, [Access Denied: MD XT DL], [Allowed: RD LU]
    [Program Version: 3]
    [V3 Procedure: ACCESS (4)]
    Status: NFS3_OK (0)
    obj_attributes  Directory mode: 0755 uid: 0 gid: 0
        attributes_follow: value follows (1)
        attributes  Directory mode: 0755 uid: 0 gid: 0
            Type: Directory (2)
            Mode: 0755, S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IXGRP, S_IROTH, S_IXOTH
                .... .... .... .... .... 0... .... .... = S_ISUID: No
                .... .... .... .... .... .0.. .... .... = S_ISGID: No
                .... .... .... .... .... ..0. .... .... = S_ISVTX: No
                .... .... .... .... .... ...1 .... .... = S_IRUSR: Yes
                .... .... .... .... .... .... 1... .... = S_IWUSR: Yes
                .... .... .... .... .... .... .1.. .... = S_IXUSR: Yes
                .... .... .... .... .... .... ..1. .... = S_IRGRP: Yes
                .... .... .... .... .... .... ...0 .... = S_IWGRP: No
                .... .... .... .... .... .... .... 1... = S_IXGRP: Yes
                .... .... .... .... .... .... .... .1.. = S_IROTH: Yes
                .... .... .... .... .... .... .... ..0. = S_IWOTH: No
                .... .... .... .... .... .... .... ...1 = S_IXOTH: Yes
            nlink: 2
            uid: 0
            gid: 0
            size: 6
            used: 0
            rdev: 0,0
                specdata1: 0
                specdata2: 0
            fsid: 0x0000000000000000 (0)
            fileid: 268662440
            atime: Apr  9, 2025 10:55:52.125736320 PDT
                seconds: 1744221352
                nano seconds: 125736320
            mtime: Jul 16, 2024 17:00:00.000000000 PDT
                seconds: 1721174400
                nano seconds: 0
            ctime: Mar 24, 2025 08:12:31.959723726 PDT
                seconds: 1742829151
                nano seconds: 959723726
    Access rights (of requested): 0x00000003
        .... ...1 = 0x001 READ: allowed
        .... ..1. = 0x002 LOOKUP: allowed
        .... .0.. = 0x004 MODIFY: *Access Denied*
        .... 0... = 0x008 EXTEND: *Access Denied*
        ...0 .... = 0x010 DELETE: *Access Denied*
*/

unsigned char nfs3_access_root_reply_packet_data[] = {
	0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0xe8, 0xff, 0x1e, 0xd4, 0xff, 0x61,
	0x08, 0x00, 0x08, 0x00, 0x45, 0x00, 0x00, 0xb0, 0x1f, 0x71, 0x40, 0x00,
	0x40, 0x06, 0x94, 0x89, 0xc0, 0xa8, 0x02, 0x7f, 0xc0, 0xa8, 0x02, 0x7e,
	0x08, 0x01, 0x03, 0xe6, 0xb4, 0x78, 0xd0, 0x8a, 0x5e, 0xf7, 0xb0, 0x96,
	0x80, 0x18, 0x01, 0xfd, 0xad, 0xd7, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a,
	0x13, 0x21, 0x13, 0xb1, 0x11, 0x91, 0xaa, 0xad, 0x80, 0x00, 0x00, 0x78,
	0xb5, 0x46, 0x84, 0x2e, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x01, 0xed, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x10, 0x03, 0x76, 0xa8, 0x67, 0xf6, 0xb4, 0xa8,
	0x07, 0x7e, 0x95, 0x80, 0x66, 0x97, 0x09, 0x80, 0x00, 0x00, 0x00, 0x00,
	0x67, 0xe1, 0x76, 0x5f, 0x39, 0x34, 0x38, 0xce, 0x00, 0x00, 0x00, 0x03
};

/*
Frame 4341: 180 bytes on wire (1440 bits), 180 bytes captured (1440 bits) on interface any, id 0
    Section number: 1
    Interface id: 0 (any)
        Interface name: any
    Encapsulation type: Linux cooked-mode capture v1 (25)
    Arrival Time: Apr  9, 2025 10:57:41.671332945 PDT
    UTC Arrival Time: Apr  9, 2025 17:57:41.671332945 UTC
    Epoch Arrival Time: 1744221461.671332945
    [Time shift for this packet: 0.000000000 seconds]
    [Time delta from previous captured frame: 0.000138113 seconds]
    [Time delta from previous displayed frame: 0.000138113 seconds]
    [Time since reference or first frame: 32.436314679 seconds]
    Frame Number: 4341
    Frame Length: 180 bytes (1440 bits)
    Capture Length: 180 bytes (1440 bits)
    [Frame is marked: False]
    [Frame is ignored: False]
    [Protocols in frame: sll:ethertype:ip:tcp:rpc:nfs]
Linux cooked capture v1
    Packet type: Sent by us (4)
    Link-layer address type: Ethernet (1)
    Link-layer address length: 6
    Source: IP3CenturyIn_3e:f7:8b (84:47:09:3e:f7:8b)
    Unused: 080a
    Protocol: IPv4 (0x0800)
Internet Protocol Version 4, Src: 192.168.2.126, Dst: 192.168.2.127
    0100 .... = Version: 4
    .... 0101 = Header Length: 20 bytes (5)
    Differentiated Services Field: 0x00 (DSCP: CS0, ECN: Not-ECT)
        0000 00.. = Differentiated Services Codepoint: Default (0)
        .... ..00 = Explicit Congestion Notification: Not ECN-Capable Transport (0)
    Total Length: 164
    Identification: 0xbc58 (48216)
    010. .... = Flags: 0x2, Don't fragment
        0... .... = Reserved bit: Not set
        .1.. .... = Don't fragment: Set
        ..0. .... = More fragments: Not set
    ...0 0000 0000 0000 = Fragment Offset: 0
    Time to Live: 64
    Protocol: TCP (6)
    Header Checksum: 0xf7ad [validation disabled]
    [Header checksum status: Unverified]
    Source Address: 192.168.2.126
    Destination Address: 192.168.2.127
    [Stream index: 1]
Transmission Control Protocol, Src Port: 998, Dst Port: 2049, Seq: 937, Ack: 1001, Len: 112
    Source Port: 998
    Destination Port: 2049
    [Stream index: 11]
    [Stream Packet Number: 32]
    [Conversation completeness: Incomplete, DATA (15)]
        ..0. .... = RST: Absent
        ...0 .... = FIN: Absent
        .... 1... = Data: Present
        .... .1.. = ACK: Present
        .... ..1. = SYN-ACK: Present
        .... ...1 = SYN: Present
        [Completeness Flags: ··DASS]
    [TCP Segment Len: 112]
    Sequence Number: 937    (relative sequence number)
    Sequence Number (raw): 1593290902
    [Next Sequence Number: 1049    (relative sequence number)]
    Acknowledgment Number: 1001    (relative ack number)
    Acknowledgment number (raw): 3027816710
    1000 .... = Header Length: 32 bytes (8)
    Flags: 0x018 (PSH, ACK)
        000. .... .... = Reserved: Not set
        ...0 .... .... = Accurate ECN: Not set
        .... 0... .... = Congestion Window Reduced: Not set
        .... .0.. .... = ECN-Echo: Not set
        .... ..0. .... = Urgent: Not set
        .... ...1 .... = Acknowledgment: Set
        .... .... 1... = Push: Set
        .... .... .0.. = Reset: Not set
        .... .... ..0. = Syn: Not set
        .... .... ...0 = Fin: Not set
        [TCP Flags: ·······AP···]
    Window: 501
    [Calculated window size: 64128]
    [Window size scaling factor: 128]
    Checksum: 0xb92f [unverified]
    [Checksum Status: Unverified]
    Urgent Pointer: 0
    Options: (12 bytes), No-Operation (NOP), No-Operation (NOP), Timestamps
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - Timestamps: TSval 294759085, TSecr 320934833
            Kind: Time Stamp Option (8)
            Length: 10
            Timestamp value: 294759085
            Timestamp echo reply: 320934833
    [Timestamps]
        [Time since first frame in this TCP stream: 4.021230151 seconds]
        [Time since previous frame in this TCP stream: 0.000138113 seconds]
    [SEQ/ACK analysis]
        [This is an ACK to the segment in frame: 4340]
        [The RTT to ACK the segment was: 0.000138113 seconds]
        [iRTT: 0.000127844 seconds]
        [Bytes in flight: 112]
        [Bytes sent since last PSH flag: 112]
    TCP payload (112 bytes)
Remote Procedure Call, Type:Call XID:0xb646842e
    Fragment header: Last fragment, 108 bytes
        1... .... .... .... .... .... .... .... = Last Fragment: Yes
        .000 0000 0000 0000 0000 0000 0110 1100 = Fragment Length: 108
    XID: 0xb646842e (3058074670)
    Message Type: Call (0)
    RPC Version: 2
    Program: NFS (100003)
    Program Version: 3
    Procedure: READDIRPLUS (17)
    Credentials
        Flavor: AUTH_UNIX (1)
        Length: 32
        Stamp: 0x00000000
        Machine Name: garbo
            length: 5
            contents: garbo
            fill bytes: opaque data
        UID: 1066
        GID: 10
        Auxiliary GIDs (1) [10]
            GID: 10
    Verifier
        Flavor: AUTH_NULL (0)
        Length: 0
Network File System, READDIRPLUS Call FH: 0x62d40c52
    [Program Version: 3]
    [V3 Procedure: READDIRPLUS (17)]
    dir
        length: 8
        [hash (CRC-32): 0x62d40c52]
        FileHandle: 0100010000000000
    cookie: 0
    Verifier: Opaque Data
    dircount: 4096
    maxcount: 4096
*/

unsigned char nfs3_readdir_empty_request_packet_data[] = {
	0x00, 0x04, 0x00, 0x01, 0x00, 0x06, 0x84, 0x47, 0x09, 0x3e, 0xf7, 0x8b,
	0x08, 0x0a, 0x08, 0x00, 0x45, 0x00, 0x00, 0xa4, 0xbc, 0x58, 0x40, 0x00,
	0x40, 0x06, 0xf7, 0xad, 0xc0, 0xa8, 0x02, 0x7e, 0xc0, 0xa8, 0x02, 0x7f,
	0x03, 0xe6, 0x08, 0x01, 0x5e, 0xf7, 0xb0, 0x96, 0xb4, 0x78, 0xd1, 0x06,
	0x80, 0x18, 0x01, 0xf5, 0xb9, 0x2f, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a,
	0x11, 0x91, 0xaa, 0xad, 0x13, 0x21, 0x13, 0xb1, 0x80, 0x00, 0x00, 0x6c,
	0xb6, 0x46, 0x84, 0x2e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x01, 0x86, 0xa3, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x11,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x05, 0x67, 0x61, 0x72, 0x62, 0x6f, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00
};

/*
Frame 4342: 372 bytes on wire (2976 bits), 372 bytes captured (2976 bits) on interface any, id 0
    Section number: 1
    Interface id: 0 (any)
        Interface name: any
    Encapsulation type: Linux cooked-mode capture v1 (25)
    Arrival Time: Apr  9, 2025 10:57:41.671628218 PDT
    UTC Arrival Time: Apr  9, 2025 17:57:41.671628218 UTC
    Epoch Arrival Time: 1744221461.671628218
    [Time shift for this packet: 0.000000000 seconds]
    [Time delta from previous captured frame: 0.000295273 seconds]
    [Time delta from previous displayed frame: 0.000295273 seconds]
    [Time since reference or first frame: 32.436609952 seconds]
    Frame Number: 4342
    Frame Length: 372 bytes (2976 bits)
    Capture Length: 372 bytes (2976 bits)
    [Frame is marked: False]
    [Frame is ignored: False]
    [Protocols in frame: sll:ethertype:ip:tcp:rpc:nfs]
Linux cooked capture v1
    Packet type: Unicast to us (0)
    Link-layer address type: Ethernet (1)
    Link-layer address length: 6
    Source: AZWTechnolog_4:ff:61 (e8:ff:1e:d4:ff:61)
    Unused: b482
    Protocol: IPv4 (0x0800)
Internet Protocol Version 4, Src: 192.168.2.127, Dst: 192.168.2.126
    0100 .... = Version: 4
    .... 0101 = Header Length: 20 bytes (5)
    Differentiated Services Field: 0x00 (DSCP: CS0, ECN: Not-ECT)
        0000 00.. = Differentiated Services Codepoint: Default (0)
        .... ..00 = Explicit Congestion Notification: Not ECN-Capable Transport (0)
    Total Length: 356
    Identification: 0x1f72 (8050)
    010. .... = Flags: 0x2, Don't fragment
        0... .... = Reserved bit: Not set
        .1.. .... = Don't fragment: Set
        ..0. .... = More fragments: Not set
    ...0 0000 0000 0000 = Fragment Offset: 0
    Time to Live: 64
    Protocol: TCP (6)
    Header Checksum: 0x93d4 [validation disabled]
    [Header checksum status: Unverified]
    Source Address: 192.168.2.127
    Destination Address: 192.168.2.126
    [Stream index: 1]
Transmission Control Protocol, Src Port: 2049, Dst Port: 998, Seq: 1001, Ack: 1049, Len: 304
    Source Port: 2049
    Destination Port: 998
    [Stream index: 11]
    [Stream Packet Number: 33]
    [Conversation completeness: Incomplete, DATA (15)]
        ..0. .... = RST: Absent
        ...0 .... = FIN: Absent
        .... 1... = Data: Present
        .... .1.. = ACK: Present
        .... ..1. = SYN-ACK: Present
        .... ...1 = SYN: Present
        [Completeness Flags: ··DASS]
    [TCP Segment Len: 304]
    Sequence Number: 1001    (relative sequence number)
    Sequence Number (raw): 3027816710
    [Next Sequence Number: 1305    (relative sequence number)]
    Acknowledgment Number: 1049    (relative ack number)
    Acknowledgment number (raw): 1593291014
    1000 .... = Header Length: 32 bytes (8)
    Flags: 0x018 (PSH, ACK)
        000. .... .... = Reserved: Not set
        ...0 .... .... = Accurate ECN: Not set
        .... 0... .... = Congestion Window Reduced: Not set
        .... .0.. .... = ECN-Echo: Not set
        .... ..0. .... = Urgent: Not set
        .... ...1 .... = Acknowledgment: Set
        .... .... 1... = Push: Set
        .... .... .0.. = Reset: Not set
        .... .... ..0. = Syn: Not set
        .... .... ...0 = Fin: Not set
        [TCP Flags: ·······AP···]
    Window: 509
    [Calculated window size: 65152]
    [Window size scaling factor: 128]
    Checksum: 0xc077 [unverified]
    [Checksum Status: Unverified]
    Urgent Pointer: 0
    Options: (12 bytes), No-Operation (NOP), No-Operation (NOP), Timestamps
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - Timestamps: TSval 320934833, TSecr 294759085
            Kind: Time Stamp Option (8)
            Length: 10
            Timestamp value: 320934833
            Timestamp echo reply: 294759085
    [Timestamps]
        [Time since first frame in this TCP stream: 4.021525424 seconds]
        [Time since previous frame in this TCP stream: 0.000295273 seconds]
    [SEQ/ACK analysis]
        [This is an ACK to the segment in frame: 4341]
        [The RTT to ACK the segment was: 0.000295273 seconds]
        [iRTT: 0.000127844 seconds]
        [Bytes in flight: 304]
        [Bytes sent since last PSH flag: 304]
    TCP payload (304 bytes)
Remote Procedure Call, Type:Reply XID:0xb646842e
    Fragment header: Last fragment, 300 bytes
        1... .... .... .... .... .... .... .... = Last Fragment: Yes
        .000 0000 0000 0000 0000 0001 0010 1100 = Fragment Length: 300
    XID: 0xb646842e (3058074670)
    Message Type: Reply (1)
    [Program: NFS (100003)]
    [Program Version: 3]
    [Procedure: READDIRPLUS (17)]
    Reply State: accepted (0)
    [This is a reply to a request in frame 4341]
    [Time from request: 0.000295273 seconds]
    Verifier
        Flavor: AUTH_NULL (0)
        Length: 0
    Accept State: RPC executed successfully (0)
Network File System, READDIRPLUS Reply
    [Program Version: 3]
    [V3 Procedure: READDIRPLUS (17)]
    Status: NFS3_OK (0)
    dir_attributes  Directory mode: 0755 uid: 0 gid: 0
        attributes_follow: value follows (1)
        attributes  Directory mode: 0755 uid: 0 gid: 0
            Type: Directory (2)
            Mode: 0755, S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IXGRP, S_IROTH, S_IXOTH
                .... .... .... .... .... 0... .... .... = S_ISUID: No
                .... .... .... .... .... .0.. .... .... = S_ISGID: No
                .... .... .... .... .... ..0. .... .... = S_ISVTX: No
                .... .... .... .... .... ...1 .... .... = S_IRUSR: Yes
                .... .... .... .... .... .... 1... .... = S_IWUSR: Yes
                .... .... .... .... .... .... .1.. .... = S_IXUSR: Yes
                .... .... .... .... .... .... ..1. .... = S_IRGRP: Yes
                .... .... .... .... .... .... ...0 .... = S_IWGRP: No
                .... .... .... .... .... .... .... 1... = S_IXGRP: Yes
                .... .... .... .... .... .... .... .1.. = S_IROTH: Yes
                .... .... .... .... .... .... .... ..0. = S_IWOTH: No
                .... .... .... .... .... .... .... ...1 = S_IXOTH: Yes
            nlink: 2
            uid: 0
            gid: 0
            size: 6
            used: 0
            rdev: 0,0
                specdata1: 0
                specdata2: 0
            fsid: 0x0000000000000000 (0)
            fileid: 268662440
            atime: Apr  9, 2025 10:55:52.125736320 PDT
                seconds: 1744221352
                nano seconds: 125736320
            mtime: Jul 16, 2024 17:00:00.000000000 PDT
                seconds: 1721174400
                nano seconds: 0
            ctime: Mar 24, 2025 08:12:31.959723726 PDT
                seconds: 1742829151
                nano seconds: 959723726
    Verifier: Opaque Data
    Value Follows: Yes
    Entry: name .
        File ID: 268662440
        Name: .
            length: 1
            contents: .
            fill bytes: opaque data
        Cookie: 10
        name_attributes  Directory mode: 0755 uid: 0 gid: 0
            attributes_follow: value follows (1)
            attributes  Directory mode: 0755 uid: 0 gid: 0
                Type: Directory (2)
                Mode: 0755, S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IXGRP, S_IROTH, S_IXOTH
                    .... .... .... .... .... 0... .... .... = S_ISUID: No
                    .... .... .... .... .... .0.. .... .... = S_ISGID: No
                    .... .... .... .... .... ..0. .... .... = S_ISVTX: No
                    .... .... .... .... .... ...1 .... .... = S_IRUSR: Yes
                    .... .... .... .... .... .... 1... .... = S_IWUSR: Yes
                    .... .... .... .... .... .... .1.. .... = S_IXUSR: Yes
                    .... .... .... .... .... .... ..1. .... = S_IRGRP: Yes
                    .... .... .... .... .... .... ...0 .... = S_IWGRP: No
                    .... .... .... .... .... .... .... 1... = S_IXGRP: Yes
                    .... .... .... .... .... .... .... .1.. = S_IROTH: Yes
                    .... .... .... .... .... .... .... ..0. = S_IWOTH: No
                    .... .... .... .... .... .... .... ...1 = S_IXOTH: Yes
                nlink: 2
                uid: 0
                gid: 0
                size: 6
                used: 0
                rdev: 0,0
                    specdata1: 0
                    specdata2: 0
                fsid: 0x0000000000000000 (0)
                fileid: 268662440
                atime: Apr  9, 2025 10:55:52.125736320 PDT
                    seconds: 1744221352
                    nano seconds: 125736320
                mtime: Jul 16, 2024 17:00:00.000000000 PDT
                    seconds: 1721174400
                    nano seconds: 0
                ctime: Mar 24, 2025 08:12:31.959723726 PDT
                    seconds: 1742829151
                    nano seconds: 959723726
        name_handle
            handle_follow: value follows (1)
            handle
                length: 8
                [hash (CRC-32): 0x62d40c52]
                FileHandle: 0100010000000000
    Value Follows: Yes
    Entry: name ..
        File ID: 128
        Name: ..
            length: 2
            contents: ..
            fill bytes: opaque data
        Cookie: 512
        name_attributes
            attributes_follow: no value (0)
        name_handle
            handle_follow: no value (0)
    Value Follows: No
    EOF: 1
*/

unsigned char nfs3_readdir_empty_reply_packet_data[] = {
	0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0xe8, 0xff, 0x1e, 0xd4, 0xff, 0x61,
	0xb4, 0x82, 0x08, 0x00, 0x45, 0x00, 0x01, 0x64, 0x1f, 0x72, 0x40, 0x00,
	0x40, 0x06, 0x93, 0xd4, 0xc0, 0xa8, 0x02, 0x7f, 0xc0, 0xa8, 0x02, 0x7e,
	0x08, 0x01, 0x03, 0xe6, 0xb4, 0x78, 0xd1, 0x06, 0x5e, 0xf7, 0xb1, 0x06,
	0x80, 0x18, 0x01, 0xfd, 0xc0, 0x77, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a,
	0x13, 0x21, 0x13, 0xb1, 0x11, 0x91, 0xaa, 0xad, 0x80, 0x00, 0x01, 0x2c,
	0xb6, 0x46, 0x84, 0x2e, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x01, 0xed, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x10, 0x03, 0x76, 0xa8, 0x67, 0xf6, 0xb4, 0xa8,
	0x07, 0x7e, 0x95, 0x80, 0x66, 0x97, 0x09, 0x80, 0x00, 0x00, 0x00, 0x00,
	0x67, 0xe1, 0x76, 0x5f, 0x39, 0x34, 0x38, 0xce, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x10, 0x03, 0x76, 0xa8, 0x00, 0x00, 0x00, 0x01, 0x2e, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x01, 0xed, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x03, 0x76, 0xa8,
	0x67, 0xf6, 0xb4, 0xa8, 0x07, 0x7e, 0x95, 0x80, 0x66, 0x97, 0x09, 0x80,
	0x00, 0x00, 0x00, 0x00, 0x67, 0xe1, 0x76, 0x5f, 0x39, 0x34, 0x38, 0xce,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x02, 0x2e, 0x2e, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01
};

/*
Frame 8077: 160 bytes on wire (1280 bits), 160 bytes captured (1280 bits) on interface any, id 0
    Section number: 1
    Interface id: 0 (any)
        Interface name: any
    Encapsulation type: Linux cooked-mode capture v1 (25)
    Arrival Time: Apr  9, 2025 10:58:24.847960315 PDT
    UTC Arrival Time: Apr  9, 2025 17:58:24.847960315 UTC
    Epoch Arrival Time: 1744221504.847960315
    [Time shift for this packet: 0.000000000 seconds]
    [Time delta from previous captured frame: 0.002495306 seconds]
    [Time delta from previous displayed frame: 9.597330074 seconds]
    [Time since reference or first frame: 75.612942049 seconds]
    Frame Number: 8077
    Frame Length: 160 bytes (1280 bits)
    Capture Length: 160 bytes (1280 bits)
    [Frame is marked: False]
    [Frame is ignored: False]
    [Protocols in frame: sll:ethertype:ip:tcp:rpc:nfs]
Linux cooked capture v1
    Packet type: Sent by us (4)
    Link-layer address type: Ethernet (1)
    Link-layer address length: 6
    Source: IP3CenturyIn_3e:f7:8b (84:47:09:3e:f7:8b)
    Unused: f78b
    Protocol: IPv4 (0x0800)
Internet Protocol Version 4, Src: 192.168.2.126, Dst: 192.168.2.127
    0100 .... = Version: 4
    .... 0101 = Header Length: 20 bytes (5)
    Differentiated Services Field: 0x00 (DSCP: CS0, ECN: Not-ECT)
        0000 00.. = Differentiated Services Codepoint: Default (0)
        .... ..00 = Explicit Congestion Notification: Not ECN-Capable Transport (0)
    Total Length: 144
    Identification: 0xbc69 (48233)
    010. .... = Flags: 0x2, Don't fragment
        0... .... = Reserved bit: Not set
        .1.. .... = Don't fragment: Set
        ..0. .... = More fragments: Not set
    ...0 0000 0000 0000 = Fragment Offset: 0
    Time to Live: 64
    Protocol: TCP (6)
    Header Checksum: 0xf7b0 [validation disabled]
    [Header checksum status: Unverified]
    Source Address: 192.168.2.126
    Destination Address: 192.168.2.127
    [Stream index: 1]
Transmission Control Protocol, Src Port: 998, Dst Port: 2049, Seq: 5337, Ack: 2593, Len: 92
    Source Port: 998
    Destination Port: 2049
    [Stream index: 11]
    [Stream Packet Number: 59]
    [Conversation completeness: Incomplete, DATA (15)]
        ..0. .... = RST: Absent
        ...0 .... = FIN: Absent
        .... 1... = Data: Present
        .... .1.. = ACK: Present
        .... ..1. = SYN-ACK: Present
        .... ...1 = SYN: Present
        [Completeness Flags: ··DASS]
    [TCP Segment Len: 92]
    Sequence Number: 5337    (relative sequence number)
    Sequence Number (raw): 1593295302
    [Next Sequence Number: 5429    (relative sequence number)]
    Acknowledgment Number: 2593    (relative ack number)
    Acknowledgment number (raw): 3027818302
    1000 .... = Header Length: 32 bytes (8)
    Flags: 0x018 (PSH, ACK)
        000. .... .... = Reserved: Not set
        ...0 .... .... = Accurate ECN: Not set
        .... 0... .... = Congestion Window Reduced: Not set
        .... .0.. .... = ECN-Echo: Not set
        .... ..0. .... = Urgent: Not set
        .... ...1 .... = Acknowledgment: Set
        .... .... 1... = Push: Set
        .... .... .0.. = Reset: Not set
        .... .... ..0. = Syn: Not set
        .... .... ...0 = Fin: Not set
        [TCP Flags: ·······AP···]
    Window: 501
    [Calculated window size: 64128]
    [Window size scaling factor: 128]
    Checksum: 0x8c08 [unverified]
    [Checksum Status: Unverified]
    Urgent Pointer: 0
    Options: (12 bytes), No-Operation (NOP), No-Operation (NOP), Timestamps
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - Timestamps: TSval 294802262, TSecr 320968412
            Kind: Time Stamp Option (8)
            Length: 10
            Timestamp value: 294802262
            Timestamp echo reply: 320968412
    [Timestamps]
        [Time since first frame in this TCP stream: 47.197857521 seconds]
        [Time since previous frame in this TCP stream: 9.557196363 seconds]
    [SEQ/ACK analysis]
        [iRTT: 0.000127844 seconds]
        [Bytes in flight: 92]
        [Bytes sent since last PSH flag: 92]
    TCP payload (92 bytes)
Remote Procedure Call, Type:Call XID:0xc046842e
    Fragment header: Last fragment, 88 bytes
        1... .... .... .... .... .... .... .... = Last Fragment: Yes
        .000 0000 0000 0000 0000 0000 0101 1000 = Fragment Length: 88
    XID: 0xc046842e (3225846830)
    Message Type: Call (0)
    RPC Version: 2
    Program: NFS (100003)
    Program Version: 3
    Procedure: ACCESS (4)
    Credentials
        Flavor: AUTH_UNIX (1)
        Length: 32
        Stamp: 0x00000000
        Machine Name: garbo
            length: 5
            contents: garbo
            fill bytes: opaque data
        UID: 1066
        GID: 10
        Auxiliary GIDs (1) [10]
            GID: 10
    Verifier
        Flavor: AUTH_NULL (0)
        Length: 0
Network File System, ACCESS Call, FH: 0x62d40c52, [Check: RD LU MD XT DL]
    [Program Version: 3]
    [V3 Procedure: ACCESS (4)]
    object
        length: 8
        [hash (CRC-32): 0x62d40c52]
        FileHandle: 0100010000000000
    Check access: 0x0000001f
        .... ...1 = 0x001 READ: allowed?
        .... ..1. = 0x002 LOOKUP: allowed?
        .... .1.. = 0x004 MODIFY: allowed?
        .... 1... = 0x008 EXTEND: allowed?
        ...1 .... = 0x010 DELETE: allowed?
*/

unsigned char nfs3_access_request_packet_data[] = {
	0x00, 0x04, 0x00, 0x01, 0x00, 0x06, 0x84, 0x47, 0x09, 0x3e, 0xf7, 0x8b,
	0xf7, 0x8b, 0x08, 0x00, 0x45, 0x00, 0x00, 0x90, 0xbc, 0x69, 0x40, 0x00,
	0x40, 0x06, 0xf7, 0xb0, 0xc0, 0xa8, 0x02, 0x7e, 0xc0, 0xa8, 0x02, 0x7f,
	0x03, 0xe6, 0x08, 0x01, 0x5e, 0xf7, 0xc1, 0xc6, 0xb4, 0x78, 0xd7, 0x3e,
	0x80, 0x18, 0x01, 0xf5, 0x8c, 0x08, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a,
	0x11, 0x92, 0x53, 0x56, 0x13, 0x21, 0x96, 0xdc, 0x80, 0x00, 0x00, 0x58,
	0xc0, 0x46, 0x84, 0x2e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x01, 0x86, 0xa3, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x05, 0x67, 0x61, 0x72, 0x62, 0x6f, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x1f
};

/*
Frame 8078: 192 bytes on wire (1536 bits), 192 bytes captured (1536 bits) on interface any, id 0
    Section number: 1
    Interface id: 0 (any)
        Interface name: any
    Encapsulation type: Linux cooked-mode capture v1 (25)
    Arrival Time: Apr  9, 2025 10:58:24.848565669 PDT
    UTC Arrival Time: Apr  9, 2025 17:58:24.848565669 UTC
    Epoch Arrival Time: 1744221504.848565669
    [Time shift for this packet: 0.000000000 seconds]
    [Time delta from previous captured frame: 0.000605354 seconds]
    [Time delta from previous displayed frame: 0.000605354 seconds]
    [Time since reference or first frame: 75.613547403 seconds]
    Frame Number: 8078
    Frame Length: 192 bytes (1536 bits)
    Capture Length: 192 bytes (1536 bits)
    [Frame is marked: False]
    [Frame is ignored: False]
    [Protocols in frame: sll:ethertype:ip:tcp:rpc:nfs]
Linux cooked capture v1
    Packet type: Unicast to us (0)
    Link-layer address type: Ethernet (1)
    Link-layer address length: 6
    Source: AZWTechnolog_4:ff:61 (e8:ff:1e:d4:ff:61)
    Unused: 8d21
    Protocol: IPv4 (0x0800)
Internet Protocol Version 4, Src: 192.168.2.127, Dst: 192.168.2.126
    0100 .... = Version: 4
    .... 0101 = Header Length: 20 bytes (5)
    Differentiated Services Field: 0x00 (DSCP: CS0, ECN: Not-ECT)
        0000 00.. = Differentiated Services Codepoint: Default (0)
        .... ..00 = Explicit Congestion Notification: Not ECN-Capable Transport (0)
    Total Length: 176
    Identification: 0x1f7d (8061)
    010. .... = Flags: 0x2, Don't fragment
        0... .... = Reserved bit: Not set
        .1.. .... = Don't fragment: Set
        ..0. .... = More fragments: Not set
    ...0 0000 0000 0000 = Fragment Offset: 0
    Time to Live: 64
    Protocol: TCP (6)
    Header Checksum: 0x947d [validation disabled]
    [Header checksum status: Unverified]
    Source Address: 192.168.2.127
    Destination Address: 192.168.2.126
    [Stream index: 1]
Transmission Control Protocol, Src Port: 2049, Dst Port: 998, Seq: 2593, Ack: 5429, Len: 124
    Source Port: 2049
    Destination Port: 998
    [Stream index: 11]
    [Stream Packet Number: 60]
    [Conversation completeness: Incomplete, DATA (15)]
        ..0. .... = RST: Absent
        ...0 .... = FIN: Absent
        .... 1... = Data: Present
        .... .1.. = ACK: Present
        .... ..1. = SYN-ACK: Present
        .... ...1 = SYN: Present
        [Completeness Flags: ··DASS]
    [TCP Segment Len: 124]
    Sequence Number: 2593    (relative sequence number)
    Sequence Number (raw): 3027818302
    [Next Sequence Number: 2717    (relative sequence number)]
    Acknowledgment Number: 5429    (relative ack number)
    Acknowledgment number (raw): 1593295394
    1000 .... = Header Length: 32 bytes (8)
    Flags: 0x018 (PSH, ACK)
        000. .... .... = Reserved: Not set
        ...0 .... .... = Accurate ECN: Not set
        .... 0... .... = Congestion Window Reduced: Not set
        .... .0.. .... = ECN-Echo: Not set
        .... ..0. .... = Urgent: Not set
        .... ...1 .... = Acknowledgment: Set
        .... .... 1... = Push: Set
        .... .... .0.. = Reset: Not set
        .... .... ..0. = Syn: Not set
        .... .... ...0 = Fin: Not set
        [TCP Flags: ·······AP···]
    Window: 576
    [Calculated window size: 73728]
    [Window size scaling factor: 128]
    Checksum: 0x35cc [unverified]
    [Checksum Status: Unverified]
    Urgent Pointer: 0
    Options: (12 bytes), No-Operation (NOP), No-Operation (NOP), Timestamps
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - Timestamps: TSval 320978010, TSecr 294802262
            Kind: Time Stamp Option (8)
            Length: 10
            Timestamp value: 320978010
            Timestamp echo reply: 294802262
    [Timestamps]
        [Time since first frame in this TCP stream: 47.198462875 seconds]
        [Time since previous frame in this TCP stream: 0.000605354 seconds]
    [SEQ/ACK analysis]
        [This is an ACK to the segment in frame: 8077]
        [The RTT to ACK the segment was: 0.000605354 seconds]
        [iRTT: 0.000127844 seconds]
        [Bytes in flight: 124]
        [Bytes sent since last PSH flag: 124]
    TCP payload (124 bytes)
Remote Procedure Call, Type:Reply XID:0xc046842e
    Fragment header: Last fragment, 120 bytes
        1... .... .... .... .... .... .... .... = Last Fragment: Yes
        .000 0000 0000 0000 0000 0000 0111 1000 = Fragment Length: 120
    XID: 0xc046842e (3225846830)
    Message Type: Reply (1)
    [Program: NFS (100003)]
    [Program Version: 3]
    [Procedure: ACCESS (4)]
    Reply State: accepted (0)
    [This is a reply to a request in frame 8077]
    [Time from request: 0.000605354 seconds]
    Verifier
        Flavor: AUTH_NULL (0)
        Length: 0
    Accept State: RPC executed successfully (0)
Network File System, ACCESS Reply, [Allowed: RD LU MD XT DL]
    [Program Version: 3]
    [V3 Procedure: ACCESS (4)]
    Status: NFS3_OK (0)
    obj_attributes  Directory mode: 0777 uid: 0 gid: 0
        attributes_follow: value follows (1)
        attributes  Directory mode: 0777 uid: 0 gid: 0
            Type: Directory (2)
            Mode: 0777, S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP, S_IXGRP, S_IROTH, S_IWOTH, S_IXOTH
                .... .... .... .... .... 0... .... .... = S_ISUID: No
                .... .... .... .... .... .0.. .... .... = S_ISGID: No
                .... .... .... .... .... ..0. .... .... = S_ISVTX: No
                .... .... .... .... .... ...1 .... .... = S_IRUSR: Yes
                .... .... .... .... .... .... 1... .... = S_IWUSR: Yes
                .... .... .... .... .... .... .1.. .... = S_IXUSR: Yes
                .... .... .... .... .... .... ..1. .... = S_IRGRP: Yes
                .... .... .... .... .... .... ...1 .... = S_IWGRP: Yes
                .... .... .... .... .... .... .... 1... = S_IXGRP: Yes
                .... .... .... .... .... .... .... .1.. = S_IROTH: Yes
                .... .... .... .... .... .... .... ..1. = S_IWOTH: Yes
                .... .... .... .... .... .... .... ...1 = S_IXOTH: Yes
            nlink: 2
            uid: 0
            gid: 0
            size: 20
            used: 0
            rdev: 0,0
                specdata1: 0
                specdata2: 0
            fsid: 0x0000000000000000 (0)
            fileid: 268662440
            atime: Apr  9, 2025 10:55:52.125736320 PDT
                seconds: 1744221352
                nano seconds: 125736320
            mtime: Apr  9, 2025 10:58:15.240891424 PDT
                seconds: 1744221495
                nano seconds: 240891424
            ctime: Apr  9, 2025 10:58:15.240891424 PDT
                seconds: 1744221495
                nano seconds: 240891424
    Access rights (of requested): 0x0000001f
        .... ...1 = 0x001 READ: allowed
        .... ..1. = 0x002 LOOKUP: allowed
        .... .1.. = 0x004 MODIFY: allowed
        .... 1... = 0x008 EXTEND: allowed
        ...1 .... = 0x010 DELETE: allowed
*/

unsigned char nfs3_access_reply_packet_data[] = {
	0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0xe8, 0xff, 0x1e, 0xd4, 0xff, 0x61,
	0x8d, 0x21, 0x08, 0x00, 0x45, 0x00, 0x00, 0xb0, 0x1f, 0x7d, 0x40, 0x00,
	0x40, 0x06, 0x94, 0x7d, 0xc0, 0xa8, 0x02, 0x7f, 0xc0, 0xa8, 0x02, 0x7e,
	0x08, 0x01, 0x03, 0xe6, 0xb4, 0x78, 0xd7, 0x3e, 0x5e, 0xf7, 0xc2, 0x22,
	0x80, 0x18, 0x02, 0x40, 0x35, 0xcc, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a,
	0x13, 0x21, 0xbc, 0x5a, 0x11, 0x92, 0x53, 0x56, 0x80, 0x00, 0x00, 0x78,
	0xc0, 0x46, 0x84, 0x2e, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x01, 0xff, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x10, 0x03, 0x76, 0xa8, 0x67, 0xf6, 0xb4, 0xa8,
	0x07, 0x7e, 0x95, 0x80, 0x67, 0xf6, 0xb5, 0x37, 0x0e, 0x5b, 0xb6, 0x20,
	0x67, 0xf6, 0xb5, 0x37, 0x0e, 0x5b, 0xb6, 0x20, 0x00, 0x00, 0x00, 0x1f
};

/*
Frame 8080: 168 bytes on wire (1344 bits), 168 bytes captured (1344 bits) on interface any, id 0
    Section number: 1
    Interface id: 0 (any)
        Interface name: any
    Encapsulation type: Linux cooked-mode capture v1 (25)
    Arrival Time: Apr  9, 2025 10:58:24.849039853 PDT
    UTC Arrival Time: Apr  9, 2025 17:58:24.849039853 UTC
    Epoch Arrival Time: 1744221504.849039853
    [Time shift for this packet: 0.000000000 seconds]
    [Time delta from previous captured frame: 0.000396426 seconds]
    [Time delta from previous displayed frame: 0.000474184 seconds]
    [Time since reference or first frame: 75.614021587 seconds]
    Frame Number: 8080
    Frame Length: 168 bytes (1344 bits)
    Capture Length: 168 bytes (1344 bits)
    [Frame is marked: False]
    [Frame is ignored: False]
    [Protocols in frame: sll:ethertype:ip:tcp:rpc:nfs]
Linux cooked capture v1
    Packet type: Sent by us (4)
    Link-layer address type: Ethernet (1)
    Link-layer address length: 6
    Source: IP3CenturyIn_3e:f7:8b (84:47:09:3e:f7:8b)
    Unused: 0800
    Protocol: IPv4 (0x0800)
Internet Protocol Version 4, Src: 192.168.2.126, Dst: 192.168.2.127
    0100 .... = Version: 4
    .... 0101 = Header Length: 20 bytes (5)
    Differentiated Services Field: 0x00 (DSCP: CS0, ECN: Not-ECT)
        0000 00.. = Differentiated Services Codepoint: Default (0)
        .... ..00 = Explicit Congestion Notification: Not ECN-Capable Transport (0)
    Total Length: 152
    Identification: 0xbc6b (48235)
    010. .... = Flags: 0x2, Don't fragment
        0... .... = Reserved bit: Not set
        .1.. .... = Don't fragment: Set
        ..0. .... = More fragments: Not set
    ...0 0000 0000 0000 = Fragment Offset: 0
    Time to Live: 64
    Protocol: TCP (6)
    Header Checksum: 0xf7a6 [validation disabled]
    [Header checksum status: Unverified]
    Source Address: 192.168.2.126
    Destination Address: 192.168.2.127
    [Stream index: 1]
Transmission Control Protocol, Src Port: 998, Dst Port: 2049, Seq: 5429, Ack: 2717, Len: 100
    Source Port: 998
    Destination Port: 2049
    [Stream index: 11]
    [Stream Packet Number: 62]
    [Conversation completeness: Incomplete, DATA (15)]
        ..0. .... = RST: Absent
        ...0 .... = FIN: Absent
        .... 1... = Data: Present
        .... .1.. = ACK: Present
        .... ..1. = SYN-ACK: Present
        .... ...1 = SYN: Present
        [Completeness Flags: ··DASS]
    [TCP Segment Len: 100]
    Sequence Number: 5429    (relative sequence number)
    Sequence Number (raw): 1593295394
    [Next Sequence Number: 5529    (relative sequence number)]
    Acknowledgment Number: 2717    (relative ack number)
    Acknowledgment number (raw): 3027818426
    1000 .... = Header Length: 32 bytes (8)
    Flags: 0x018 (PSH, ACK)
        000. .... .... = Reserved: Not set
        ...0 .... .... = Accurate ECN: Not set
        .... 0... .... = Congestion Window Reduced: Not set
        .... .0.. .... = ECN-Echo: Not set
        .... ..0. .... = Urgent: Not set
        .... ...1 .... = Acknowledgment: Set
        .... .... 1... = Push: Set
        .... .... .0.. = Reset: Not set
        .... .... ..0. = Syn: Not set
        .... .... ...0 = Fin: Not set
        [TCP Flags: ·······AP···]
    Window: 501
    [Calculated window size: 64128]
    [Window size scaling factor: 128]
    Checksum: 0xa94a [unverified]
    [Checksum Status: Unverified]
    Urgent Pointer: 0
    Options: (12 bytes), No-Operation (NOP), No-Operation (NOP), Timestamps
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - Timestamps: TSval 294802263, TSecr 320978010
            Kind: Time Stamp Option (8)
            Length: 10
            Timestamp value: 294802263
            Timestamp echo reply: 320978010
    [Timestamps]
        [Time since first frame in this TCP stream: 47.198937059 seconds]
        [Time since previous frame in this TCP stream: 0.000396426 seconds]
    [SEQ/ACK analysis]
        [iRTT: 0.000127844 seconds]
        [Bytes in flight: 100]
        [Bytes sent since last PSH flag: 100]
    TCP payload (100 bytes)
Remote Procedure Call, Type:Call XID:0xc146842e
    Fragment header: Last fragment, 96 bytes
        1... .... .... .... .... .... .... .... = Last Fragment: Yes
        .000 0000 0000 0000 0000 0000 0110 0000 = Fragment Length: 96
    XID: 0xc146842e (3242624046)
    Message Type: Call (0)
    RPC Version: 2
    Program: NFS (100003)
    Program Version: 3
    Procedure: GETATTR (1)
    Credentials
        Flavor: AUTH_UNIX (1)
        Length: 32
        Stamp: 0x00000000
        Machine Name: garbo
            length: 5
            contents: garbo
            fill bytes: opaque data
        UID: 1066
        GID: 10
        Auxiliary GIDs (1) [10]
            GID: 10
    Verifier
        Flavor: AUTH_NULL (0)
        Length: 0
Network File System, GETATTR Call FH: 0xd93439fb
    [Program Version: 3]
    [V3 Procedure: GETATTR (1)]
    object
        length: 20
        [hash (CRC-32): 0xd93439fb]
        FileHandle: 0100018100000000a96d0810000000009f226a4b
*/

unsigned char nfs3_getattr_file_request_packet_data[] = {
	0x00, 0x04, 0x00, 0x01, 0x00, 0x06, 0x84, 0x47, 0x09, 0x3e, 0xf7, 0x8b,
	0x08, 0x00, 0x08, 0x00, 0x45, 0x00, 0x00, 0x98, 0xbc, 0x6b, 0x40, 0x00,
	0x40, 0x06, 0xf7, 0xa6, 0xc0, 0xa8, 0x02, 0x7e, 0xc0, 0xa8, 0x02, 0x7f,
	0x03, 0xe6, 0x08, 0x01, 0x5e, 0xf7, 0xc2, 0x22, 0xb4, 0x78, 0xd7, 0xba,
	0x80, 0x18, 0x01, 0xf5, 0xa9, 0x4a, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a,
	0x11, 0x92, 0x53, 0x57, 0x13, 0x21, 0xbc, 0x5a, 0x80, 0x00, 0x00, 0x60,
	0xc1, 0x46, 0x84, 0x2e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x01, 0x86, 0xa3, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x05, 0x67, 0x61, 0x72, 0x62, 0x6f, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x14, 0x01, 0x00, 0x01, 0x81, 0x00, 0x00, 0x00, 0x00,
	0xa9, 0x6d, 0x08, 0x10, 0x00, 0x00, 0x00, 0x00, 0x9f, 0x22, 0x6a, 0x4b
};

/*
Frame 8081: 184 bytes on wire (1472 bits), 184 bytes captured (1472 bits) on interface any, id 0
    Section number: 1
    Interface id: 0 (any)
        Interface name: any
    Encapsulation type: Linux cooked-mode capture v1 (25)
    Arrival Time: Apr  9, 2025 10:58:24.849301221 PDT
    UTC Arrival Time: Apr  9, 2025 17:58:24.849301221 UTC
    Epoch Arrival Time: 1744221504.849301221
    [Time shift for this packet: 0.000000000 seconds]
    [Time delta from previous captured frame: 0.000261368 seconds]
    [Time delta from previous displayed frame: 0.000261368 seconds]
    [Time since reference or first frame: 75.614282955 seconds]
    Frame Number: 8081
    Frame Length: 184 bytes (1472 bits)
    Capture Length: 184 bytes (1472 bits)
    [Frame is marked: False]
    [Frame is ignored: False]
    [Protocols in frame: sll:ethertype:ip:tcp:rpc:nfs]
Linux cooked capture v1
    Packet type: Unicast to us (0)
    Link-layer address type: Ethernet (1)
    Link-layer address length: 6
    Source: AZWTechnolog_4:ff:61 (e8:ff:1e:d4:ff:61)
    Unused: ecb2
    Protocol: IPv4 (0x0800)
Internet Protocol Version 4, Src: 192.168.2.127, Dst: 192.168.2.126
    0100 .... = Version: 4
    .... 0101 = Header Length: 20 bytes (5)
    Differentiated Services Field: 0x00 (DSCP: CS0, ECN: Not-ECT)
        0000 00.. = Differentiated Services Codepoint: Default (0)
        .... ..00 = Explicit Congestion Notification: Not ECN-Capable Transport (0)
    Total Length: 168
    Identification: 0x1f7e (8062)
    010. .... = Flags: 0x2, Don't fragment
        0... .... = Reserved bit: Not set
        .1.. .... = Don't fragment: Set
        ..0. .... = More fragments: Not set
    ...0 0000 0000 0000 = Fragment Offset: 0
    Time to Live: 64
    Protocol: TCP (6)
    Header Checksum: 0x9484 [validation disabled]
    [Header checksum status: Unverified]
    Source Address: 192.168.2.127
    Destination Address: 192.168.2.126
    [Stream index: 1]
Transmission Control Protocol, Src Port: 2049, Dst Port: 998, Seq: 2717, Ack: 5529, Len: 116
    Source Port: 2049
    Destination Port: 998
    [Stream index: 11]
    [Stream Packet Number: 63]
    [Conversation completeness: Incomplete, DATA (15)]
        ..0. .... = RST: Absent
        ...0 .... = FIN: Absent
        .... 1... = Data: Present
        .... .1.. = ACK: Present
        .... ..1. = SYN-ACK: Present
        .... ...1 = SYN: Present
        [Completeness Flags: ··DASS]
    [TCP Segment Len: 116]
    Sequence Number: 2717    (relative sequence number)
    Sequence Number (raw): 3027818426
    [Next Sequence Number: 2833    (relative sequence number)]
    Acknowledgment Number: 5529    (relative ack number)
    Acknowledgment number (raw): 1593295494
    1000 .... = Header Length: 32 bytes (8)
    Flags: 0x018 (PSH, ACK)
        000. .... .... = Reserved: Not set
        ...0 .... .... = Accurate ECN: Not set
        .... 0... .... = Congestion Window Reduced: Not set
        .... .0.. .... = ECN-Echo: Not set
        .... ..0. .... = Urgent: Not set
        .... ...1 .... = Acknowledgment: Set
        .... .... 1... = Push: Set
        .... .... .0.. = Reset: Not set
        .... .... ..0. = Syn: Not set
        .... .... ...0 = Fin: Not set
        [TCP Flags: ·······AP···]
    Window: 576
    [Calculated window size: 73728]
    [Window size scaling factor: 128]
    Checksum: 0x1f4f [unverified]
    [Checksum Status: Unverified]
    Urgent Pointer: 0
    Options: (12 bytes), No-Operation (NOP), No-Operation (NOP), Timestamps
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - Timestamps: TSval 320978011, TSecr 294802263
            Kind: Time Stamp Option (8)
            Length: 10
            Timestamp value: 320978011
            Timestamp echo reply: 294802263
    [Timestamps]
        [Time since first frame in this TCP stream: 47.199198427 seconds]
        [Time since previous frame in this TCP stream: 0.000261368 seconds]
    [SEQ/ACK analysis]
        [This is an ACK to the segment in frame: 8080]
        [The RTT to ACK the segment was: 0.000261368 seconds]
        [iRTT: 0.000127844 seconds]
        [Bytes in flight: 116]
        [Bytes sent since last PSH flag: 116]
    TCP payload (116 bytes)
Remote Procedure Call, Type:Reply XID:0xc146842e
    Fragment header: Last fragment, 112 bytes
        1... .... .... .... .... .... .... .... = Last Fragment: Yes
        .000 0000 0000 0000 0000 0000 0111 0000 = Fragment Length: 112
    XID: 0xc146842e (3242624046)
    Message Type: Reply (1)
    [Program: NFS (100003)]
    [Program Version: 3]
    [Procedure: GETATTR (1)]
    Reply State: accepted (0)
    [This is a reply to a request in frame 8080]
    [Time from request: 0.000261368 seconds]
    Verifier
        Flavor: AUTH_NULL (0)
        Length: 0
    Accept State: RPC executed successfully (0)
Network File System, GETATTR Reply  Regular File mode: 0644 uid: 65534 gid: 65534
    [Program Version: 3]
    [V3 Procedure: GETATTR (1)]
    Status: NFS3_OK (0)
    obj_attributes  Regular File mode: 0644 uid: 65534 gid: 65534
        Type: Regular File (1)
        Mode: 0644, S_IRUSR, S_IWUSR, S_IRGRP, S_IROTH
            .... .... .... .... .... 0... .... .... = S_ISUID: No
            .... .... .... .... .... .0.. .... .... = S_ISGID: No
            .... .... .... .... .... ..0. .... .... = S_ISVTX: No
            .... .... .... .... .... ...1 .... .... = S_IRUSR: Yes
            .... .... .... .... .... .... 1... .... = S_IWUSR: Yes
            .... .... .... .... .... .... .0.. .... = S_IXUSR: No
            .... .... .... .... .... .... ..1. .... = S_IRGRP: Yes
            .... .... .... .... .... .... ...0 .... = S_IWGRP: No
            .... .... .... .... .... .... .... 0... = S_IXGRP: No
            .... .... .... .... .... .... .... .1.. = S_IROTH: Yes
            .... .... .... .... .... .... .... ..0. = S_IWOTH: No
            .... .... .... .... .... .... .... ...0 = S_IXOTH: No
        nlink: 1
        uid: 65534
        gid: 65534
        size: 3334
        used: 4096
        rdev: 0,0
            specdata1: 0
            specdata2: 0
        fsid: 0x0000000000000000 (0)
        fileid: 268987817
        atime: Apr  9, 2025 10:58:15.243891406 PDT
            seconds: 1744221495
            nano seconds: 243891406
        mtime: Apr  9, 2025 10:58:15.244891400 PDT
            seconds: 1744221495
            nano seconds: 244891400
        ctime: Apr  9, 2025 10:58:15.244891400 PDT
            seconds: 1744221495
            nano seconds: 244891400
*/

unsigned char nfs3_getattr_file_reply_packet_data[] = {
	0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0xe8, 0xff, 0x1e, 0xd4, 0xff, 0x61,
	0xec, 0xb2, 0x08, 0x00, 0x45, 0x00, 0x00, 0xa8, 0x1f, 0x7e, 0x40, 0x00,
	0x40, 0x06, 0x94, 0x84, 0xc0, 0xa8, 0x02, 0x7f, 0xc0, 0xa8, 0x02, 0x7e,
	0x08, 0x01, 0x03, 0xe6, 0xb4, 0x78, 0xd7, 0xba, 0x5e, 0xf7, 0xc2, 0x86,
	0x80, 0x18, 0x02, 0x40, 0x1f, 0x4f, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a,
	0x13, 0x21, 0xbc, 0x5b, 0x11, 0x92, 0x53, 0x57, 0x80, 0x00, 0x00, 0x70,
	0xc1, 0x46, 0x84, 0x2e, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0xa4,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00, 0xff, 0xfe,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x06, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x10, 0x08, 0x6d, 0xa9, 0x67, 0xf6, 0xb5, 0x37, 0x0e, 0x89, 0x7c, 0xce,
	0x67, 0xf6, 0xb5, 0x37, 0x0e, 0x98, 0xbf, 0x08, 0x67, 0xf6, 0xb5, 0x37,
	0x0e, 0x98, 0xbf, 0x08
};

/*
Frame 8718: 156 bytes on wire (1248 bits), 156 bytes captured (1248 bits) on interface any, id 0
    Section number: 1
    Interface id: 0 (any)
        Interface name: any
    Encapsulation type: Linux cooked-mode capture v1 (25)
    Arrival Time: Apr  9, 2025 10:58:31.131032059 PDT
    UTC Arrival Time: Apr  9, 2025 17:58:31.131032059 UTC
    Epoch Arrival Time: 1744221511.131032059
    [Time shift for this packet: 0.000000000 seconds]
    [Time delta from previous captured frame: 0.003206392 seconds]
    [Time delta from previous displayed frame: 6.281730838 seconds]
    [Time since reference or first frame: 81.896013793 seconds]
    Frame Number: 8718
    Frame Length: 156 bytes (1248 bits)
    Capture Length: 156 bytes (1248 bits)
    [Frame is marked: False]
    [Frame is ignored: False]
    [Protocols in frame: sll:ethertype:ip:tcp:rpc:nfs]
Linux cooked capture v1
    Packet type: Sent by us (4)
    Link-layer address type: Ethernet (1)
    Link-layer address length: 6
    Source: IP3CenturyIn_3e:f7:8b (84:47:09:3e:f7:8b)
    Unused: 0000
    Protocol: IPv4 (0x0800)
Internet Protocol Version 4, Src: 192.168.2.126, Dst: 192.168.2.127
    0100 .... = Version: 4
    .... 0101 = Header Length: 20 bytes (5)
    Differentiated Services Field: 0x00 (DSCP: CS0, ECN: Not-ECT)
        0000 00.. = Differentiated Services Codepoint: Default (0)
        .... ..00 = Explicit Congestion Notification: Not ECN-Capable Transport (0)
    Total Length: 140
    Identification: 0xbc6d (48237)
    010. .... = Flags: 0x2, Don't fragment
        0... .... = Reserved bit: Not set
        .1.. .... = Don't fragment: Set
        ..0. .... = More fragments: Not set
    ...0 0000 0000 0000 = Fragment Offset: 0
    Time to Live: 64
    Protocol: TCP (6)
    Header Checksum: 0xf7b0 [validation disabled]
    [Header checksum status: Unverified]
    Source Address: 192.168.2.126
    Destination Address: 192.168.2.127
    [Stream index: 1]
Transmission Control Protocol, Src Port: 998, Dst Port: 2049, Seq: 5529, Ack: 2833, Len: 88
    Source Port: 998
    Destination Port: 2049
    [Stream index: 11]
    [Stream Packet Number: 65]
    [Conversation completeness: Incomplete, DATA (15)]
        ..0. .... = RST: Absent
        ...0 .... = FIN: Absent
        .... 1... = Data: Present
        .... .1.. = ACK: Present
        .... ..1. = SYN-ACK: Present
        .... ...1 = SYN: Present
        [Completeness Flags: ··DASS]
    [TCP Segment Len: 88]
    Sequence Number: 5529    (relative sequence number)
    Sequence Number (raw): 1593295494
    [Next Sequence Number: 5617    (relative sequence number)]
    Acknowledgment Number: 2833    (relative ack number)
    Acknowledgment number (raw): 3027818542
    1000 .... = Header Length: 32 bytes (8)
    Flags: 0x018 (PSH, ACK)
        000. .... .... = Reserved: Not set
        ...0 .... .... = Accurate ECN: Not set
        .... 0... .... = Congestion Window Reduced: Not set
        .... .0.. .... = ECN-Echo: Not set
        .... ..0. .... = Urgent: Not set
        .... ...1 .... = Acknowledgment: Set
        .... .... 1... = Push: Set
        .... .... .0.. = Reset: Not set
        .... .... ..0. = Syn: Not set
        .... .... ...0 = Fin: Not set
        [TCP Flags: ·······AP···]
    Window: 501
    [Calculated window size: 64128]
    [Window size scaling factor: 128]
    Checksum: 0x4a78 [unverified]
    [Checksum Status: Unverified]
    Urgent Pointer: 0
    Options: (12 bytes), No-Operation (NOP), No-Operation (NOP), Timestamps
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - Timestamps: TSval 294808545, TSecr 320978011
            Kind: Time Stamp Option (8)
            Length: 10
            Timestamp value: 294808545
            Timestamp echo reply: 320978011
    [Timestamps]
        [Time since first frame in this TCP stream: 53.480929265 seconds]
        [Time since previous frame in this TCP stream: 6.241257590 seconds]
    [SEQ/ACK analysis]
        [iRTT: 0.000127844 seconds]
        [Bytes in flight: 88]
        [Bytes sent since last PSH flag: 88]
    TCP payload (88 bytes)
Remote Procedure Call, Type:Call XID:0xc246842e
    Fragment header: Last fragment, 84 bytes
        1... .... .... .... .... .... .... .... = Last Fragment: Yes
        .000 0000 0000 0000 0000 0000 0101 0100 = Fragment Length: 84
    XID: 0xc246842e (3259401262)
    Message Type: Call (0)
    RPC Version: 2
    Program: NFS (100003)
    Program Version: 3
    Procedure: GETATTR (1)
    Credentials
        Flavor: AUTH_UNIX (1)
        Length: 32
        Stamp: 0x00000000
        Machine Name: garbo
            length: 5
            contents: garbo
            fill bytes: opaque data
        UID: 1066
        GID: 10
        Auxiliary GIDs (1) [10]
            GID: 10
    Verifier
        Flavor: AUTH_NULL (0)
        Length: 0
Network File System, GETATTR Call FH: 0x62d40c52
    [Program Version: 3]
    [V3 Procedure: GETATTR (1)]
    object
        length: 8
        [hash (CRC-32): 0x62d40c52]
        FileHandle: 0100010000000000
*/

unsigned char nfs3_getattr_dir_request_packet_data[] = {
	0x00, 0x04, 0x00, 0x01, 0x00, 0x06, 0x84, 0x47, 0x09, 0x3e, 0xf7, 0x8b,
	0x00, 0x00, 0x08, 0x00, 0x45, 0x00, 0x00, 0x8c, 0xbc, 0x6d, 0x40, 0x00,
	0x40, 0x06, 0xf7, 0xb0, 0xc0, 0xa8, 0x02, 0x7e, 0xc0, 0xa8, 0x02, 0x7f,
	0x03, 0xe6, 0x08, 0x01, 0x5e, 0xf7, 0xc2, 0x86, 0xb4, 0x78, 0xd8, 0x2e,
	0x80, 0x18, 0x01, 0xf5, 0x4a, 0x78, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a,
	0x11, 0x92, 0x6b, 0xe1, 0x13, 0x21, 0xbc, 0x5b, 0x80, 0x00, 0x00, 0x54,
	0xc2, 0x46, 0x84, 0x2e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x01, 0x86, 0xa3, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x05, 0x67, 0x61, 0x72, 0x62, 0x6f, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
};

/*
Frame 8719: 184 bytes on wire (1472 bits), 184 bytes captured (1472 bits) on interface any, id 0
    Section number: 1
    Interface id: 0 (any)
        Interface name: any
    Encapsulation type: Linux cooked-mode capture v1 (25)
    Arrival Time: Apr  9, 2025 10:58:31.131704170 PDT
    UTC Arrival Time: Apr  9, 2025 17:58:31.131704170 UTC
    Epoch Arrival Time: 1744221511.131704170
    [Time shift for this packet: 0.000000000 seconds]
    [Time delta from previous captured frame: 0.000672111 seconds]
    [Time delta from previous displayed frame: 0.000672111 seconds]
    [Time since reference or first frame: 81.896685904 seconds]
    Frame Number: 8719
    Frame Length: 184 bytes (1472 bits)
    Capture Length: 184 bytes (1472 bits)
    [Frame is marked: False]
    [Frame is ignored: False]
    [Protocols in frame: sll:ethertype:ip:tcp:rpc:nfs]
Linux cooked capture v1
    Packet type: Unicast to us (0)
    Link-layer address type: Ethernet (1)
    Link-layer address length: 6
    Source: AZWTechnolog_4:ff:61 (e8:ff:1e:d4:ff:61)
    Unused: 38bf
    Protocol: IPv4 (0x0800)
Internet Protocol Version 4, Src: 192.168.2.127, Dst: 192.168.2.126
    0100 .... = Version: 4
    .... 0101 = Header Length: 20 bytes (5)
    Differentiated Services Field: 0x00 (DSCP: CS0, ECN: Not-ECT)
        0000 00.. = Differentiated Services Codepoint: Default (0)
        .... ..00 = Explicit Congestion Notification: Not ECN-Capable Transport (0)
    Total Length: 168
    Identification: 0x1f7f (8063)
    010. .... = Flags: 0x2, Don't fragment
        0... .... = Reserved bit: Not set
        .1.. .... = Don't fragment: Set
        ..0. .... = More fragments: Not set
    ...0 0000 0000 0000 = Fragment Offset: 0
    Time to Live: 64
    Protocol: TCP (6)
    Header Checksum: 0x9483 [validation disabled]
    [Header checksum status: Unverified]
    Source Address: 192.168.2.127
    Destination Address: 192.168.2.126
    [Stream index: 1]
Transmission Control Protocol, Src Port: 2049, Dst Port: 998, Seq: 2833, Ack: 5617, Len: 116
    Source Port: 2049
    Destination Port: 998
    [Stream index: 11]
    [Stream Packet Number: 66]
    [Conversation completeness: Incomplete, DATA (15)]
        ..0. .... = RST: Absent
        ...0 .... = FIN: Absent
        .... 1... = Data: Present
        .... .1.. = ACK: Present
        .... ..1. = SYN-ACK: Present
        .... ...1 = SYN: Present
        [Completeness Flags: ··DASS]
    [TCP Segment Len: 116]
    Sequence Number: 2833    (relative sequence number)
    Sequence Number (raw): 3027818542
    [Next Sequence Number: 2949    (relative sequence number)]
    Acknowledgment Number: 5617    (relative ack number)
    Acknowledgment number (raw): 1593295582
    1000 .... = Header Length: 32 bytes (8)
    Flags: 0x018 (PSH, ACK)
        000. .... .... = Reserved: Not set
        ...0 .... .... = Accurate ECN: Not set
        .... 0... .... = Congestion Window Reduced: Not set
        .... .0.. .... = ECN-Echo: Not set
        .... ..0. .... = Urgent: Not set
        .... ...1 .... = Acknowledgment: Set
        .... .... 1... = Push: Set
        .... .... .0.. = Reset: Not set
        .... .... ..0. = Syn: Not set
        .... .... ...0 = Fin: Not set
        [TCP Flags: ·······AP···]
    Window: 576
    [Calculated window size: 73728]
    [Window size scaling factor: 128]
    Checksum: 0x013a [unverified]
    [Checksum Status: Unverified]
    Urgent Pointer: 0
    Options: (12 bytes), No-Operation (NOP), No-Operation (NOP), Timestamps
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - Timestamps: TSval 320984293, TSecr 294808545
            Kind: Time Stamp Option (8)
            Length: 10
            Timestamp value: 320984293
            Timestamp echo reply: 294808545
    [Timestamps]
        [Time since first frame in this TCP stream: 53.481601376 seconds]
        [Time since previous frame in this TCP stream: 0.000672111 seconds]
    [SEQ/ACK analysis]
        [This is an ACK to the segment in frame: 8718]
        [The RTT to ACK the segment was: 0.000672111 seconds]
        [iRTT: 0.000127844 seconds]
        [Bytes in flight: 116]
        [Bytes sent since last PSH flag: 116]
    TCP payload (116 bytes)
Remote Procedure Call, Type:Reply XID:0xc246842e
    Fragment header: Last fragment, 112 bytes
        1... .... .... .... .... .... .... .... = Last Fragment: Yes
        .000 0000 0000 0000 0000 0000 0111 0000 = Fragment Length: 112
    XID: 0xc246842e (3259401262)
    Message Type: Reply (1)
    [Program: NFS (100003)]
    [Program Version: 3]
    [Procedure: GETATTR (1)]
    Reply State: accepted (0)
    [This is a reply to a request in frame 8718]
    [Time from request: 0.000672111 seconds]
    Verifier
        Flavor: AUTH_NULL (0)
        Length: 0
    Accept State: RPC executed successfully (0)
Network File System, GETATTR Reply  Directory mode: 0777 uid: 0 gid: 0
    [Program Version: 3]
    [V3 Procedure: GETATTR (1)]
    Status: NFS3_OK (0)
    obj_attributes  Directory mode: 0777 uid: 0 gid: 0
        Type: Directory (2)
        Mode: 0777, S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP, S_IXGRP, S_IROTH, S_IWOTH, S_IXOTH
            .... .... .... .... .... 0... .... .... = S_ISUID: No
            .... .... .... .... .... .0.. .... .... = S_ISGID: No
            .... .... .... .... .... ..0. .... .... = S_ISVTX: No
            .... .... .... .... .... ...1 .... .... = S_IRUSR: Yes
            .... .... .... .... .... .... 1... .... = S_IWUSR: Yes
            .... .... .... .... .... .... .1.. .... = S_IXUSR: Yes
            .... .... .... .... .... .... ..1. .... = S_IRGRP: Yes
            .... .... .... .... .... .... ...1 .... = S_IWGRP: Yes
            .... .... .... .... .... .... .... 1... = S_IXGRP: Yes
            .... .... .... .... .... .... .... .1.. = S_IROTH: Yes
            .... .... .... .... .... .... .... ..1. = S_IWOTH: Yes
            .... .... .... .... .... .... .... ...1 = S_IXOTH: Yes
        nlink: 2
        uid: 0
        gid: 0
        size: 20
        used: 0
        rdev: 0,0
            specdata1: 0
            specdata2: 0
        fsid: 0x0000000000000000 (0)
        fileid: 268662440
        atime: Apr  9, 2025 10:55:52.125736320 PDT
            seconds: 1744221352
            nano seconds: 125736320
        mtime: Apr  9, 2025 10:58:15.240891424 PDT
            seconds: 1744221495
            nano seconds: 240891424
        ctime: Apr  9, 2025 10:58:15.240891424 PDT
            seconds: 1744221495
            nano seconds: 240891424
*/

unsigned char nfs3_getattr_dir_reply_packet_data[] = {
	0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0xe8, 0xff, 0x1e, 0xd4, 0xff, 0x61,
	0x38, 0xbf, 0x08, 0x00, 0x45, 0x00, 0x00, 0xa8, 0x1f, 0x7f, 0x40, 0x00,
	0x40, 0x06, 0x94, 0x83, 0xc0, 0xa8, 0x02, 0x7f, 0xc0, 0xa8, 0x02, 0x7e,
	0x08, 0x01, 0x03, 0xe6, 0xb4, 0x78, 0xd8, 0x2e, 0x5e, 0xf7, 0xc2, 0xde,
	0x80, 0x18, 0x02, 0x40, 0x01, 0x3a, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a,
	0x13, 0x21, 0xd4, 0xe5, 0x11, 0x92, 0x6b, 0xe1, 0x80, 0x00, 0x00, 0x70,
	0xc2, 0x46, 0x84, 0x2e, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x01, 0xff,
	0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x10, 0x03, 0x76, 0xa8, 0x67, 0xf6, 0xb4, 0xa8, 0x07, 0x7e, 0x95, 0x80,
	0x67, 0xf6, 0xb5, 0x37, 0x0e, 0x5b, 0xb6, 0x20, 0x67, 0xf6, 0xb5, 0x37,
	0x0e, 0x5b, 0xb6, 0x20
};

/*
Frame 8722: 180 bytes on wire (1440 bits), 180 bytes captured (1440 bits) on interface any, id 0
    Section number: 1
    Interface id: 0 (any)
        Interface name: any
    Encapsulation type: Linux cooked-mode capture v1 (25)
    Arrival Time: Apr  9, 2025 10:58:31.131945620 PDT
    UTC Arrival Time: Apr  9, 2025 17:58:31.131945620 UTC
    Epoch Arrival Time: 1744221511.131945620
    [Time shift for this packet: 0.000000000 seconds]
    [Time delta from previous captured frame: 0.000012433 seconds]
    [Time delta from previous displayed frame: 0.000241450 seconds]
    [Time since reference or first frame: 81.896927354 seconds]
    Frame Number: 8722
    Frame Length: 180 bytes (1440 bits)
    Capture Length: 180 bytes (1440 bits)
    [Frame is marked: False]
    [Frame is ignored: False]
    [Protocols in frame: sll:ethertype:ip:tcp:rpc:nfs]
Linux cooked capture v1
    Packet type: Sent by us (4)
    Link-layer address type: Ethernet (1)
    Link-layer address length: 6
    Source: IP3CenturyIn_3e:f7:8b (84:47:09:3e:f7:8b)
    Unused: 0000
    Protocol: IPv4 (0x0800)
Internet Protocol Version 4, Src: 192.168.2.126, Dst: 192.168.2.127
    0100 .... = Version: 4
    .... 0101 = Header Length: 20 bytes (5)
    Differentiated Services Field: 0x00 (DSCP: CS0, ECN: Not-ECT)
        0000 00.. = Differentiated Services Codepoint: Default (0)
        .... ..00 = Explicit Congestion Notification: Not ECN-Capable Transport (0)
    Total Length: 164
    Identification: 0xbc6f (48239)
    010. .... = Flags: 0x2, Don't fragment
        0... .... = Reserved bit: Not set
        .1.. .... = Don't fragment: Set
        ..0. .... = More fragments: Not set
    ...0 0000 0000 0000 = Fragment Offset: 0
    Time to Live: 64
    Protocol: TCP (6)
    Header Checksum: 0xf796 [validation disabled]
    [Header checksum status: Unverified]
    Source Address: 192.168.2.126
    Destination Address: 192.168.2.127
    [Stream index: 1]
Transmission Control Protocol, Src Port: 998, Dst Port: 2049, Seq: 5617, Ack: 2949, Len: 112
    Source Port: 998
    Destination Port: 2049
    [Stream index: 11]
    [Stream Packet Number: 68]
    [Conversation completeness: Incomplete, DATA (15)]
        ..0. .... = RST: Absent
        ...0 .... = FIN: Absent
        .... 1... = Data: Present
        .... .1.. = ACK: Present
        .... ..1. = SYN-ACK: Present
        .... ...1 = SYN: Present
        [Completeness Flags: ··DASS]
    [TCP Segment Len: 112]
    Sequence Number: 5617    (relative sequence number)
    Sequence Number (raw): 1593295582
    [Next Sequence Number: 5729    (relative sequence number)]
    Acknowledgment Number: 2949    (relative ack number)
    Acknowledgment number (raw): 3027818658
    1000 .... = Header Length: 32 bytes (8)
    Flags: 0x018 (PSH, ACK)
        000. .... .... = Reserved: Not set
        ...0 .... .... = Accurate ECN: Not set
        .... 0... .... = Congestion Window Reduced: Not set
        .... .0.. .... = ECN-Echo: Not set
        .... ..0. .... = Urgent: Not set
        .... ...1 .... = Acknowledgment: Set
        .... .... 1... = Push: Set
        .... .... .0.. = Reset: Not set
        .... .... ..0. = Syn: Not set
        .... .... ...0 = Fin: Not set
        [TCP Flags: ·······AP···]
    Window: 501
    [Calculated window size: 64128]
    [Window size scaling factor: 128]
    Checksum: 0x0fe1 [unverified]
    [Checksum Status: Unverified]
    Urgent Pointer: 0
    Options: (12 bytes), No-Operation (NOP), No-Operation (NOP), Timestamps
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - Timestamps: TSval 294808546, TSecr 320984293
            Kind: Time Stamp Option (8)
            Length: 10
            Timestamp value: 294808546
            Timestamp echo reply: 320984293
    [Timestamps]
        [Time since first frame in this TCP stream: 53.481842826 seconds]
        [Time since previous frame in this TCP stream: 0.000156618 seconds]
    [SEQ/ACK analysis]
        [iRTT: 0.000127844 seconds]
        [Bytes in flight: 112]
        [Bytes sent since last PSH flag: 112]
    TCP payload (112 bytes)
Remote Procedure Call, Type:Call XID:0xc346842e
    Fragment header: Last fragment, 108 bytes
        1... .... .... .... .... .... .... .... = Last Fragment: Yes
        .000 0000 0000 0000 0000 0000 0110 1100 = Fragment Length: 108
    XID: 0xc346842e (3276178478)
    Message Type: Call (0)
    RPC Version: 2
    Program: NFS (100003)
    Program Version: 3
    Procedure: READDIRPLUS (17)
    Credentials
        Flavor: AUTH_UNIX (1)
        Length: 32
        Stamp: 0x00000000
        Machine Name: garbo
            length: 5
            contents: garbo
            fill bytes: opaque data
        UID: 1066
        GID: 10
        Auxiliary GIDs (1) [10]
            GID: 10
    Verifier
        Flavor: AUTH_NULL (0)
        Length: 0
Network File System, READDIRPLUS Call FH: 0x62d40c52
    [Program Version: 3]
    [V3 Procedure: READDIRPLUS (17)]
    dir
        length: 8
        [hash (CRC-32): 0x62d40c52]
        FileHandle: 0100010000000000
    cookie: 0
    Verifier: Opaque Data
    dircount: 4096
    maxcount: 4096
*/

unsigned char nfs3_readdir_request_packet_data[] = {
	0x00, 0x04, 0x00, 0x01, 0x00, 0x06, 0x84, 0x47, 0x09, 0x3e, 0xf7, 0x8b,
	0x00, 0x00, 0x08, 0x00, 0x45, 0x00, 0x00, 0xa4, 0xbc, 0x6f, 0x40, 0x00,
	0x40, 0x06, 0xf7, 0x96, 0xc0, 0xa8, 0x02, 0x7e, 0xc0, 0xa8, 0x02, 0x7f,
	0x03, 0xe6, 0x08, 0x01, 0x5e, 0xf7, 0xc2, 0xde, 0xb4, 0x78, 0xd8, 0xa2,
	0x80, 0x18, 0x01, 0xf5, 0x0f, 0xe1, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a,
	0x11, 0x92, 0x6b, 0xe2, 0x13, 0x21, 0xd4, 0xe5, 0x80, 0x00, 0x00, 0x6c,
	0xc3, 0x46, 0x84, 0x2e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x01, 0x86, 0xa3, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x11,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x05, 0x67, 0x61, 0x72, 0x62, 0x6f, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00
};

/*
Frame 8723: 520 bytes on wire (4160 bits), 520 bytes captured (4160 bits) on interface any, id 0
    Section number: 1
    Interface id: 0 (any)
        Interface name: any
    Encapsulation type: Linux cooked-mode capture v1 (25)
    Arrival Time: Apr  9, 2025 10:58:31.132245733 PDT
    UTC Arrival Time: Apr  9, 2025 17:58:31.132245733 UTC
    Epoch Arrival Time: 1744221511.132245733
    [Time shift for this packet: 0.000000000 seconds]
    [Time delta from previous captured frame: 0.000300113 seconds]
    [Time delta from previous displayed frame: 0.000300113 seconds]
    [Time since reference or first frame: 81.897227467 seconds]
    Frame Number: 8723
    Frame Length: 520 bytes (4160 bits)
    Capture Length: 520 bytes (4160 bits)
    [Frame is marked: False]
    [Frame is ignored: False]
    [Protocols in frame: sll:ethertype:ip:tcp:rpc:nfs]
Linux cooked capture v1
    Packet type: Unicast to us (0)
    Link-layer address type: Ethernet (1)
    Link-layer address length: 6
    Source: AZWTechnolog_4:ff:61 (e8:ff:1e:d4:ff:61)
    Unused: 26d4
    Protocol: IPv4 (0x0800)
Internet Protocol Version 4, Src: 192.168.2.127, Dst: 192.168.2.126
    0100 .... = Version: 4
    .... 0101 = Header Length: 20 bytes (5)
    Differentiated Services Field: 0x00 (DSCP: CS0, ECN: Not-ECT)
        0000 00.. = Differentiated Services Codepoint: Default (0)
        .... ..00 = Explicit Congestion Notification: Not ECN-Capable Transport (0)
    Total Length: 504
    Identification: 0x1f80 (8064)
    010. .... = Flags: 0x2, Don't fragment
        0... .... = Reserved bit: Not set
        .1.. .... = Don't fragment: Set
        ..0. .... = More fragments: Not set
    ...0 0000 0000 0000 = Fragment Offset: 0
    Time to Live: 64
    Protocol: TCP (6)
    Header Checksum: 0x9332 [validation disabled]
    [Header checksum status: Unverified]
    Source Address: 192.168.2.127
    Destination Address: 192.168.2.126
    [Stream index: 1]
Transmission Control Protocol, Src Port: 2049, Dst Port: 998, Seq: 2949, Ack: 5729, Len: 452
    Source Port: 2049
    Destination Port: 998
    [Stream index: 11]
    [Stream Packet Number: 69]
    [Conversation completeness: Incomplete, DATA (15)]
        ..0. .... = RST: Absent
        ...0 .... = FIN: Absent
        .... 1... = Data: Present
        .... .1.. = ACK: Present
        .... ..1. = SYN-ACK: Present
        .... ...1 = SYN: Present
        [Completeness Flags: ··DASS]
    [TCP Segment Len: 452]
    Sequence Number: 2949    (relative sequence number)
    Sequence Number (raw): 3027818658
    [Next Sequence Number: 3401    (relative sequence number)]
    Acknowledgment Number: 5729    (relative ack number)
    Acknowledgment number (raw): 1593295694
    1000 .... = Header Length: 32 bytes (8)
    Flags: 0x018 (PSH, ACK)
        000. .... .... = Reserved: Not set
        ...0 .... .... = Accurate ECN: Not set
        .... 0... .... = Congestion Window Reduced: Not set
        .... .0.. .... = ECN-Echo: Not set
        .... ..0. .... = Urgent: Not set
        .... ...1 .... = Acknowledgment: Set
        .... .... 1... = Push: Set
        .... .... .0.. = Reset: Not set
        .... .... ..0. = Syn: Not set
        .... .... ...0 = Fin: Not set
        [TCP Flags: ·······AP···]
    Window: 576
    [Calculated window size: 73728]
    [Window size scaling factor: 128]
    Checksum: 0x750c [unverified]
    [Checksum Status: Unverified]
    Urgent Pointer: 0
    Options: (12 bytes), No-Operation (NOP), No-Operation (NOP), Timestamps
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - Timestamps: TSval 320984294, TSecr 294808546
            Kind: Time Stamp Option (8)
            Length: 10
            Timestamp value: 320984294
            Timestamp echo reply: 294808546
    [Timestamps]
        [Time since first frame in this TCP stream: 53.482142939 seconds]
        [Time since previous frame in this TCP stream: 0.000300113 seconds]
    [SEQ/ACK analysis]
        [This is an ACK to the segment in frame: 8722]
        [The RTT to ACK the segment was: 0.000300113 seconds]
        [iRTT: 0.000127844 seconds]
        [Bytes in flight: 452]
        [Bytes sent since last PSH flag: 452]
    TCP payload (452 bytes)
Remote Procedure Call, Type:Reply XID:0xc346842e
    Fragment header: Last fragment, 448 bytes
        1... .... .... .... .... .... .... .... = Last Fragment: Yes
        .000 0000 0000 0000 0000 0001 1100 0000 = Fragment Length: 448
    XID: 0xc346842e (3276178478)
    Message Type: Reply (1)
    [Program: NFS (100003)]
    [Program Version: 3]
    [Procedure: READDIRPLUS (17)]
    Reply State: accepted (0)
    [This is a reply to a request in frame 8722]
    [Time from request: 0.000300113 seconds]
    Verifier
        Flavor: AUTH_NULL (0)
        Length: 0
    Accept State: RPC executed successfully (0)
Network File System, READDIRPLUS Reply
    [Program Version: 3]
    [V3 Procedure: READDIRPLUS (17)]
    Status: NFS3_OK (0)
    dir_attributes  Directory mode: 0777 uid: 0 gid: 0
        attributes_follow: value follows (1)
        attributes  Directory mode: 0777 uid: 0 gid: 0
            Type: Directory (2)
            Mode: 0777, S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP, S_IXGRP, S_IROTH, S_IWOTH, S_IXOTH
                .... .... .... .... .... 0... .... .... = S_ISUID: No
                .... .... .... .... .... .0.. .... .... = S_ISGID: No
                .... .... .... .... .... ..0. .... .... = S_ISVTX: No
                .... .... .... .... .... ...1 .... .... = S_IRUSR: Yes
                .... .... .... .... .... .... 1... .... = S_IWUSR: Yes
                .... .... .... .... .... .... .1.. .... = S_IXUSR: Yes
                .... .... .... .... .... .... ..1. .... = S_IRGRP: Yes
                .... .... .... .... .... .... ...1 .... = S_IWGRP: Yes
                .... .... .... .... .... .... .... 1... = S_IXGRP: Yes
                .... .... .... .... .... .... .... .1.. = S_IROTH: Yes
                .... .... .... .... .... .... .... ..1. = S_IWOTH: Yes
                .... .... .... .... .... .... .... ...1 = S_IXOTH: Yes
            nlink: 2
            uid: 0
            gid: 0
            size: 20
            used: 0
            rdev: 0,0
                specdata1: 0
                specdata2: 0
            fsid: 0x0000000000000000 (0)
            fileid: 268662440
            atime: Apr  9, 2025 10:58:31.127797633 PDT
                seconds: 1744221511
                nano seconds: 127797633
            mtime: Apr  9, 2025 10:58:15.240891424 PDT
                seconds: 1744221495
                nano seconds: 240891424
            ctime: Apr  9, 2025 10:58:15.240891424 PDT
                seconds: 1744221495
                nano seconds: 240891424
    Verifier: Opaque Data
    Value Follows: Yes
    Entry: name .
        File ID: 268662440
        Name: .
            length: 1
            contents: .
            fill bytes: opaque data
        Cookie: 10
        name_attributes  Directory mode: 0777 uid: 0 gid: 0
            attributes_follow: value follows (1)
            attributes  Directory mode: 0777 uid: 0 gid: 0
                Type: Directory (2)
                Mode: 0777, S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP, S_IXGRP, S_IROTH, S_IWOTH, S_IXOTH
                    .... .... .... .... .... 0... .... .... = S_ISUID: No
                    .... .... .... .... .... .0.. .... .... = S_ISGID: No
                    .... .... .... .... .... ..0. .... .... = S_ISVTX: No
                    .... .... .... .... .... ...1 .... .... = S_IRUSR: Yes
                    .... .... .... .... .... .... 1... .... = S_IWUSR: Yes
                    .... .... .... .... .... .... .1.. .... = S_IXUSR: Yes
                    .... .... .... .... .... .... ..1. .... = S_IRGRP: Yes
                    .... .... .... .... .... .... ...1 .... = S_IWGRP: Yes
                    .... .... .... .... .... .... .... 1... = S_IXGRP: Yes
                    .... .... .... .... .... .... .... .1.. = S_IROTH: Yes
                    .... .... .... .... .... .... .... ..1. = S_IWOTH: Yes
                    .... .... .... .... .... .... .... ...1 = S_IXOTH: Yes
                nlink: 2
                uid: 0
                gid: 0
                size: 20
                used: 0
                rdev: 0,0
                    specdata1: 0
                    specdata2: 0
                fsid: 0x0000000000000000 (0)
                fileid: 268662440
                atime: Apr  9, 2025 10:58:31.127797633 PDT
                    seconds: 1744221511
                    nano seconds: 127797633
                mtime: Apr  9, 2025 10:58:15.240891424 PDT
                    seconds: 1744221495
                    nano seconds: 240891424
                ctime: Apr  9, 2025 10:58:15.240891424 PDT
                    seconds: 1744221495
                    nano seconds: 240891424
        name_handle
            handle_follow: value follows (1)
            handle
                length: 8
                [hash (CRC-32): 0x62d40c52]
                FileHandle: 0100010000000000
    Value Follows: Yes
    Entry: name ..
        File ID: 128
        Name: ..
            length: 2
            contents: ..
            fill bytes: opaque data
        Cookie: 12
        name_attributes
            attributes_follow: no value (0)
        name_handle
            handle_follow: no value (0)
    Value Follows: Yes
    Entry: name passwd
        File ID: 268987817
        Name: passwd
            length: 6
            contents: passwd
            fill bytes: opaque data
        Cookie: 512
        name_attributes  Regular File mode: 0644 uid: 65534 gid: 65534
            attributes_follow: value follows (1)
            attributes  Regular File mode: 0644 uid: 65534 gid: 65534
                Type: Regular File (1)
                Mode: 0644, S_IRUSR, S_IWUSR, S_IRGRP, S_IROTH
                    .... .... .... .... .... 0... .... .... = S_ISUID: No
                    .... .... .... .... .... .0.. .... .... = S_ISGID: No
                    .... .... .... .... .... ..0. .... .... = S_ISVTX: No
                    .... .... .... .... .... ...1 .... .... = S_IRUSR: Yes
                    .... .... .... .... .... .... 1... .... = S_IWUSR: Yes
                    .... .... .... .... .... .... .0.. .... = S_IXUSR: No
                    .... .... .... .... .... .... ..1. .... = S_IRGRP: Yes
                    .... .... .... .... .... .... ...0 .... = S_IWGRP: No
                    .... .... .... .... .... .... .... 0... = S_IXGRP: No
                    .... .... .... .... .... .... .... .1.. = S_IROTH: Yes
                    .... .... .... .... .... .... .... ..0. = S_IWOTH: No
                    .... .... .... .... .... .... .... ...0 = S_IXOTH: No
                nlink: 1
                uid: 65534
                gid: 65534
                size: 3334
                used: 4096
                rdev: 0,0
                    specdata1: 0
                    specdata2: 0
                fsid: 0x0000000000000000 (0)
                fileid: 268987817
                atime: Apr  9, 2025 10:58:15.243891406 PDT
                    seconds: 1744221495
                    nano seconds: 243891406
                mtime: Apr  9, 2025 10:58:15.244891400 PDT
                    seconds: 1744221495
                    nano seconds: 244891400
                ctime: Apr  9, 2025 10:58:15.244891400 PDT
                    seconds: 1744221495
                    nano seconds: 244891400
        name_handle
            handle_follow: value follows (1)
            handle
                length: 20
                [hash (CRC-32): 0xd93439fb]
                FileHandle: 0100018100000000a96d0810000000009f226a4b
    Value Follows: No
    EOF: 1
*/

unsigned char nfs3_readdir_reply_packet_data[] = {
	0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0xe8, 0xff, 0x1e, 0xd4, 0xff, 0x61,
	0x26, 0xd4, 0x08, 0x00, 0x45, 0x00, 0x01, 0xf8, 0x1f, 0x80, 0x40, 0x00,
	0x40, 0x06, 0x93, 0x32, 0xc0, 0xa8, 0x02, 0x7f, 0xc0, 0xa8, 0x02, 0x7e,
	0x08, 0x01, 0x03, 0xe6, 0xb4, 0x78, 0xd8, 0xa2, 0x5e, 0xf7, 0xc3, 0x4e,
	0x80, 0x18, 0x02, 0x40, 0x75, 0x0c, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a,
	0x13, 0x21, 0xd4, 0xe6, 0x11, 0x92, 0x6b, 0xe2, 0x80, 0x00, 0x01, 0xc0,
	0xc3, 0x46, 0x84, 0x2e, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x01, 0xff, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x10, 0x03, 0x76, 0xa8, 0x67, 0xf6, 0xb5, 0x47,
	0x07, 0x9e, 0x09, 0x81, 0x67, 0xf6, 0xb5, 0x37, 0x0e, 0x5b, 0xb6, 0x20,
	0x67, 0xf6, 0xb5, 0x37, 0x0e, 0x5b, 0xb6, 0x20, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x10, 0x03, 0x76, 0xa8, 0x00, 0x00, 0x00, 0x01, 0x2e, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x01, 0xff, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x03, 0x76, 0xa8,
	0x67, 0xf6, 0xb5, 0x47, 0x07, 0x9e, 0x09, 0x81, 0x67, 0xf6, 0xb5, 0x37,
	0x0e, 0x5b, 0xb6, 0x20, 0x67, 0xf6, 0xb5, 0x37, 0x0e, 0x5b, 0xb6, 0x20,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x02, 0x2e, 0x2e, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x10, 0x08, 0x6d, 0xa9, 0x00, 0x00, 0x00, 0x06, 0x70, 0x61, 0x73, 0x73,
	0x77, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0xa4,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00, 0xff, 0xfe,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d, 0x06, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x10, 0x08, 0x6d, 0xa9, 0x67, 0xf6, 0xb5, 0x37, 0x0e, 0x89, 0x7c, 0xce,
	0x67, 0xf6, 0xb5, 0x37, 0x0e, 0x98, 0xbf, 0x08, 0x67, 0xf6, 0xb5, 0x37,
	0x0e, 0x98, 0xbf, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x14,
	0x01, 0x00, 0x01, 0x81, 0x00, 0x00, 0x00, 0x00, 0xa9, 0x6d, 0x08, 0x10,
	0x00, 0x00, 0x00, 0x00, 0x9f, 0x22, 0x6a, 0x4b, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x01
};
