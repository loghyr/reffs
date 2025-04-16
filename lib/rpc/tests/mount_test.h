/*
 * SPDX-FileCopyrightText: 2025 Tom Haynes <loghyr@gmail.com>
 * SPDX-License-Identifier: GPL-2.0+
 */

/*
Frame 10764: 144 bytes on wire (1152 bits), 144 bytes captured (1152 bits) on interface any, id 0
    Section number: 1
    Interface id: 0 (any)
        Interface name: any
    Encapsulation type: Linux cooked-mode capture v1 (25)
    Arrival Time: Apr  9, 2025 10:58:47.179774148 PDT
    UTC Arrival Time: Apr  9, 2025 17:58:47.179774148 UTC
    Epoch Arrival Time: 1744221527.179774148
    [Time shift for this packet: 0.000000000 seconds]
    [Time delta from previous captured frame: 0.000055566 seconds]
    [Time delta from previous displayed frame: 3.986172946 seconds]
    [Time since reference or first frame: 97.944755882 seconds]
    Frame Number: 10764
    Frame Length: 144 bytes (1152 bits)
    Capture Length: 144 bytes (1152 bits)
    [Frame is marked: False]
    [Frame is ignored: False]
    [Protocols in frame: sll:ethertype:ip:tcp:rpc]
Linux cooked capture v1
    Packet type: Sent by us (4)
    Link-layer address type: Ethernet (1)
    Link-layer address length: 6
    Source: IP3CenturyIn_3e:f7:8b (84:47:09:3e:f7:8b)
    Unused: b4a6
    Protocol: IPv4 (0x0800)
Internet Protocol Version 4, Src: 192.168.2.126, Dst: 192.168.2.127
    0100 .... = Version: 4
    .... 0101 = Header Length: 20 bytes (5)
    Differentiated Services Field: 0x00 (DSCP: CS0, ECN: Not-ECT)
        0000 00.. = Differentiated Services Codepoint: Default (0)
        .... ..00 = Explicit Congestion Notification: Not ECN-Capable Transport (0)
    Total Length: 128
    Identification: 0x365c (13916)
    010. .... = Flags: 0x2, Don't fragment
        0... .... = Reserved bit: Not set
        .1.. .... = Don't fragment: Set
        ..0. .... = More fragments: Not set
    ...0 0000 0000 0000 = Fragment Offset: 0
    Time to Live: 64
    Protocol: TCP (6)
    Header Checksum: 0x7dce [validation disabled]
    [Header checksum status: Unverified]
    Source Address: 192.168.2.126
    Destination Address: 192.168.2.127
    [Stream index: 1]
Transmission Control Protocol, Src Port: 666, Dst Port: 20048, Seq: 1, Ack: 1, Len: 76
    Source Port: 666
    Destination Port: 20048
    [Stream index: 13]
    [Stream Packet Number: 4]
    [Conversation completeness: Incomplete, ESTABLISHED (7)]
        ..0. .... = RST: Absent
        ...0 .... = FIN: Absent
        .... 0... = Data: Absent
        .... .1.. = ACK: Present
        .... ..1. = SYN-ACK: Present
        .... ...1 = SYN: Present
        [Completeness Flags: ···ASS]
    [TCP Segment Len: 76]
    Sequence Number: 1    (relative sequence number)
    Sequence Number (raw): 2356772143
    [Next Sequence Number: 77    (relative sequence number)]
    Acknowledgment Number: 1    (relative ack number)
    Acknowledgment number (raw): 1984881069
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
    Checksum: 0x7a5c [unverified]
    [Checksum Status: Unverified]
    Urgent Pointer: 0
    Options: (12 bytes), No-Operation (NOP), No-Operation (NOP), Timestamps
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - Timestamps: TSval 294824594, TSecr 321000341
            Kind: Time Stamp Option (8)
            Length: 10
            Timestamp value: 294824594
            Timestamp echo reply: 321000341
    [Timestamps]
        [Time since first frame in this TCP stream: 0.000261749 seconds]
        [Time since previous frame in this TCP stream: 0.000055566 seconds]
    [SEQ/ACK analysis]
        [iRTT: 0.000206183 seconds]
        [Bytes in flight: 76]
        [Bytes sent since last PSH flag: 76]
    TCP payload (76 bytes)
Remote Procedure Call, Type:Call XID:0x493927f2
    Fragment header: Last fragment, 72 bytes
        1... .... .... .... .... .... .... .... = Last Fragment: Yes
        .000 0000 0000 0000 0000 0000 0100 1000 = Fragment Length: 72
    XID: 0x493927f2 (1228482546)
    Message Type: Call (0)
    RPC Version: 2
    Program: MOUNT (100005)
    Program Version: 3
    Procedure: EXPORT (5)
    Credentials
        Flavor: AUTH_UNIX (1)
        Length: 32
        Stamp: 0x67f6b557
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
Mount Service
    [Program Version: 3]
    [V3 Procedure: EXPORT (5)]

0000  00 04 00 01 00 06 84 47 09 3e f7 8b b4 a6 08 00   .......G.>......
0010  45 00 00 80 36 5c 40 00 40 06 7d ce c0 a8 02 7e   E...6\@.@.}....~
0020  c0 a8 02 7f 02 9a 4e 50 8c 79 7d 2f 76 4e e1 ad   ......NP.y}/vN..
0030  80 18 01 f6 7a 5c 00 00 01 01 08 0a 11 92 aa 92   ....z\..........
0040  13 22 13 95 80 00 00 48 49 39 27 f2 00 00 00 00   .".....HI9'.....
0050  00 00 00 02 00 01 86 a5 00 00 00 03 00 00 00 05   ................
0060  00 00 00 01 00 00 00 20 67 f6 b5 57 00 00 00 05   ....... g..W....
0070  67 61 72 62 6f 00 00 00 00 00 00 00 00 00 00 00   garbo...........
0080  00 00 00 01 00 00 00 00 00 00 00 00 00 00 00 00   ................

*/

unsigned char mount_request[] = {
	0x00, 0x04, 0x00, 0x01, 0x00, 0x06, 0x84, 0x47, 0x09, 0x3e, 0xf7, 0x8b,
	0xb4, 0xa6, 0x08, 0x00, 0x45, 0x00, 0x00, 0x80, 0x36, 0x5c, 0x40, 0x00,
	0x40, 0x06, 0x7d, 0xce, 0xc0, 0xa8, 0x02, 0x7e, 0xc0, 0xa8, 0x02, 0x7f,
	0x02, 0x9a, 0x4e, 0x50, 0x8c, 0x79, 0x7d, 0x2f, 0x76, 0x4e, 0xe1, 0xad,
	0x80, 0x18, 0x01, 0xf6, 0x7a, 0x5c, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a,
	0x11, 0x92, 0xaa, 0x92, 0x13, 0x22, 0x13, 0x95, 0x80, 0x00, 0x00, 0x48,
	0x49, 0x39, 0x27, 0xf2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
	0x00, 0x01, 0x86, 0xa5, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x05,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x20, 0x67, 0xf6, 0xb5, 0x57,
	0x00, 0x00, 0x00, 0x05, 0x67, 0x61, 0x72, 0x62, 0x6f, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/*
Frame 10766: 184 bytes on wire (1472 bits), 184 bytes captured (1472 bits) on interface any, id 0
    Section number: 1
    Interface id: 0 (any)
        Interface name: any
    Encapsulation type: Linux cooked-mode capture v1 (25)
    Arrival Time: Apr  9, 2025 10:58:47.180434687 PDT
    UTC Arrival Time: Apr  9, 2025 17:58:47.180434687 UTC
    Epoch Arrival Time: 1744221527.180434687
    [Time shift for this packet: 0.000000000 seconds]
    [Time delta from previous captured frame: 0.000509311 seconds]
    [Time delta from previous displayed frame: 0.000660539 seconds]
    [Time since reference or first frame: 97.945416421 seconds]
    Frame Number: 10766
    Frame Length: 184 bytes (1472 bits)
    Capture Length: 184 bytes (1472 bits)
    [Frame is marked: False]
    [Frame is ignored: False]
    [Protocols in frame: sll:ethertype:ip:tcp:rpc:mount]
Linux cooked capture v1
    Packet type: Unicast to us (0)
    Link-layer address type: Ethernet (1)
    Link-layer address length: 6
    Source: AZWTechnolog_4:ff:61 (e8:ff:1e:d4:ff:61)
    Unused: e295
    Protocol: IPv4 (0x0800)
Internet Protocol Version 4, Src: 192.168.2.127, Dst: 192.168.2.126
    0100 .... = Version: 4
    .... 0101 = Header Length: 20 bytes (5)
    Differentiated Services Field: 0x00 (DSCP: CS0, ECN: Not-ECT)
        0000 00.. = Differentiated Services Codepoint: Default (0)
        .... ..00 = Explicit Congestion Notification: Not ECN-Capable Transport (0)
    Total Length: 168
    Identification: 0x9ee9 (40681)
    010. .... = Flags: 0x2, Don't fragment
        0... .... = Reserved bit: Not set
        .1.. .... = Don't fragment: Set
        ..0. .... = More fragments: Not set
    ...0 0000 0000 0000 = Fragment Offset: 0
    Time to Live: 64
    Protocol: TCP (6)
    Header Checksum: 0x1519 [validation disabled]
    [Header checksum status: Unverified]
    Source Address: 192.168.2.127
    Destination Address: 192.168.2.126
    [Stream index: 1]
Transmission Control Protocol, Src Port: 20048, Dst Port: 666, Seq: 1, Ack: 77, Len: 116
    Source Port: 20048
    Destination Port: 666
    [Stream index: 13]
    [Stream Packet Number: 6]
    [Conversation completeness: Incomplete, DATA (15)]
        ..0. .... = RST: Absent
        ...0 .... = FIN: Absent
        .... 1... = Data: Present
        .... .1.. = ACK: Present
        .... ..1. = SYN-ACK: Present
        .... ...1 = SYN: Present
        [Completeness Flags: ··DASS]
    [TCP Segment Len: 116]
    Sequence Number: 1    (relative sequence number)
    Sequence Number (raw): 1984881069
    [Next Sequence Number: 117    (relative sequence number)]
    Acknowledgment Number: 77    (relative ack number)
    Acknowledgment number (raw): 2356772219
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
    Checksum: 0x8482 [unverified]
    [Checksum Status: Unverified]
    Urgent Pointer: 0
    Options: (12 bytes), No-Operation (NOP), No-Operation (NOP), Timestamps
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - No-Operation (NOP)
            Kind: No-Operation (1)
        TCP Option - Timestamps: TSval 321000342, TSecr 294824594
            Kind: Time Stamp Option (8)
            Length: 10
            Timestamp value: 321000342
            Timestamp echo reply: 294824594
    [Timestamps]
        [Time since first frame in this TCP stream: 0.000922288 seconds]
        [Time since previous frame in this TCP stream: 0.000509311 seconds]
    [SEQ/ACK analysis]
        [iRTT: 0.000206183 seconds]
        [Bytes in flight: 116]
        [Bytes sent since last PSH flag: 116]
    TCP payload (116 bytes)
Remote Procedure Call, Type:Reply XID:0x493927f2
    Fragment header: Last fragment, 112 bytes
        1... .... .... .... .... .... .... .... = Last Fragment: Yes
        .000 0000 0000 0000 0000 0000 0111 0000 = Fragment Length: 112
    XID: 0x493927f2 (1228482546)
    Message Type: Reply (1)
    [Program: MOUNT (100005)]
    [Program Version: 3]
    [Procedure: EXPORT (5)]
    Reply State: accepted (0)
    [This is a reply to a request in frame 10764]
    [Time from request: 0.000660539 seconds]
    Verifier
        Flavor: AUTH_NULL (0)
        Length: 0
    Accept State: RPC executed successfully (0)
Mount Service
    [Program Version: 3]
    [V3 Procedure: EXPORT (5)]
    Value Follows: Yes
    Export List Entry: /mirror -> 192.168.2.0/24
        Directory: /mirror
            length: 7
            contents: /mirror
            fill bytes: opaque data
        Groups
            Value Follows: Yes
            Group: 192.168.2.0/24
                length: 14
                contents: 192.168.2.0/24
                fill bytes: opaque data
            Value Follows: No
    Value Follows: Yes
    Export List Entry: /srv -> 192.168.2.0/24
        Directory: /srv
            length: 4
            contents: /srv
        Groups
            Value Follows: Yes
            Group: 192.168.2.0/24
                length: 14
                contents: 192.168.2.0/24
                fill bytes: opaque data
            Value Follows: No
    Value Follows: No

0000  00 00 00 01 00 06 e8 ff 1e d4 ff 61 e2 95 08 00   ...........a....
0010  45 00 00 a8 9e e9 40 00 40 06 15 19 c0 a8 02 7f   E.....@.@.......
0020  c0 a8 02 7e 4e 50 02 9a 76 4e e1 ad 8c 79 7d 7b   ...~NP..vN...y}{
0030  80 18 01 fd 84 82 00 00 01 01 08 0a 13 22 13 96   ............."..
0040  11 92 aa 92 80 00 00 70 49 39 27 f2 00 00 00 01   .......pI9'.....
0050  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   ................
0060  00 00 00 01 00 00 00 07 2f 6d 69 72 72 6f 72 00   ......../mirror.
0070  00 00 00 01 00 00 00 0e 31 39 32 2e 31 36 38 2e   ........192.168.
0080  32 2e 30 2f 32 34 00 00 00 00 00 00 00 00 00 01   2.0/24..........
0090  00 00 00 04 2f 73 72 76 00 00 00 01 00 00 00 0e   ..../srv........
00a0  31 39 32 2e 31 36 38 2e 32 2e 30 2f 32 34 00 00   192.168.2.0/24..
00b0  00 00 00 00 00 00 00 00                           ........

*/

unsigned char mount_reply[] = {
	0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0xe8, 0xff, 0x1e, 0xd4, 0xff, 0x61,
	0xe2, 0x95, 0x08, 0x00, 0x45, 0x00, 0x00, 0xa8, 0x9e, 0xe9, 0x40, 0x00,
	0x40, 0x06, 0x15, 0x19, 0xc0, 0xa8, 0x02, 0x7f, 0xc0, 0xa8, 0x02, 0x7e,
	0x4e, 0x50, 0x02, 0x9a, 0x76, 0x4e, 0xe1, 0xad, 0x8c, 0x79, 0x7d, 0x7b,
	0x80, 0x18, 0x01, 0xfd, 0x84, 0x82, 0x00, 0x00, 0x01, 0x01, 0x08, 0x0a,
	0x13, 0x22, 0x13, 0x96, 0x11, 0x92, 0xaa, 0x92, 0x80, 0x00, 0x00, 0x70,
	0x49, 0x39, 0x27, 0xf2, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x07, 0x2f, 0x6d, 0x69, 0x72,
	0x72, 0x6f, 0x72, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0e,
	0x31, 0x39, 0x32, 0x2e, 0x31, 0x36, 0x38, 0x2e, 0x32, 0x2e, 0x30, 0x2f,
	0x32, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x04, 0x2f, 0x73, 0x72, 0x76, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x0e, 0x31, 0x39, 0x32, 0x2e, 0x31, 0x36, 0x38, 0x2e,
	0x32, 0x2e, 0x30, 0x2f, 0x32, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00
};
