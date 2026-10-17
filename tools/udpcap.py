#!/usr/bin/env python3
"""UDP forwarder (listens on LISTEN_IP:listen_port, forwards to SERVER_IP:server_port) that records every datagram into a classic pcap file (synthetic Ethernet/IPv4/UDP headers), so gtcap can
be tested without root.   udpcap.py <listen_port> <server_port> <out.pcap> [seconds=60] [listen_ip] [server_ip]"""
import select, socket, struct, sys, time
lp, sp, out, secs = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], float(sys.argv[4]) if len(sys.argv) > 4 else 60
LISTEN_IP = sys.argv[5] if len(sys.argv) > 5 else "127.0.0.1"; SERVER_IP = sys.argv[6] if len(sys.argv) > 6 else "127.0.0.1"
f = open(out, "wb"); f.write(struct.pack("<IHHiIII", 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))
def rec(src, dst, data):
    udp = struct.pack(">HHHH", src[1], dst[1], 8 + len(data), 0) + data
    ip = struct.pack(">BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0, socket.inet_aton(src[0]), socket.inet_aton(dst[0])) + udp
    fr = b"\x00" * 6 + b"\x00" * 6 + b"\x08\x00" + ip
    t = time.time(); f.write(struct.pack("<IIII", int(t), int((t % 1) * 1e6), len(fr), len(fr)) + fr); f.flush()
lsock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); lsock.bind((LISTEN_IP, lp))
up = {}  # client addr -> upstream socket
end = time.time() + secs
while time.time() < end:
    socks = [lsock] + list(up.values()); r, _, _ = select.select(socks, [], [], 0.2)
    for s in r:
        data, addr = s.recvfrom(65535)
        if s is lsock:
            if addr not in up: up[addr] = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); up[addr].bind(("127.0.0.1", 0))
            rec(addr, (LISTEN_IP, sp), data); up[addr].sendto(data, (SERVER_IP, sp))
        else:
            caddr = [a for a, u in up.items() if u is s][0]
            rec((LISTEN_IP, sp), caddr, data); lsock.sendto(data, caddr)
