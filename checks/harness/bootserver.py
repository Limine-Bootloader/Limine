"""A boot server on a network of its own, for machines that need one behaving
otherwise than QEMU's: one host answering ARP, DHCP (RFC 2131) and TFTP
(RFC 1350, with the options of RFC 2347 it is told to support among blksize,
RFC 2348, and tsize, RFC 2349). It reaches QEMU through its socket network
backend, one Ethernet frame to a UDP datagram, and hands out the addresses
QEMU's user networking does."""

import os
import socket
import struct
import threading

CLIENT, SERVER, NETMASK = '10.0.2.15', '10.0.2.2', '255.255.255.0'
SERVER_MAC = bytes.fromhex('020000000002')
BROADCAST_MAC = b'\xff' * 6
ETH_IP, ETH_ARP = 0x0800, 0x0806
DHCP_MAGIC = bytes.fromhex('63825363')
DHCPDISCOVER, DHCPOFFER, DHCPREQUEST, DHCPACK = 1, 2, 3, 5
TFTP_RRQ, TFTP_DATA, TFTP_ACK, TFTP_ERROR, TFTP_OACK = 1, 3, 4, 5, 6
# A data packet of the largest block fills a 1500 byte MTU.
MAX_BLKSIZE = 1468


def ip_bytes(addr):
    return socket.inet_aton(addr)


def checksum(data):
    if len(data) % 2:
        data += b'\0'
    s = sum(struct.unpack(f'!{len(data) // 2}H', data))
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return ~s & 0xffff


def dhcp_options(data):
    """The options of a DHCP message after its magic cookie (RFC 2132)."""
    out, i = {}, 0
    while i < len(data) and data[i] != 255:
        if data[i] == 0:
            i += 1
            continue
        out[data[i]] = data[i + 2:i + 2 + data[i + 1]]
        i += 2 + data[i + 1]
    return out


class BootServer:
    """Serves root over TFTP and offers bootfile to whoever asks over DHCP,
    with its size in the Boot File Size option (RFC 2132, 9.5) if told to.
    requests lists the (file name, options) of each read request."""

    def __init__(self, root, bootfile, options=('blksize', 'tsize'), bootfile_size=False):
        self.root, self.bootfile, self.options = root, bootfile, set(options)
        self.bootfile_size = bootfile_size
        self.requests = []
        self.errors = []
        self.client_mac = None
        self.peer = None
        self.transfers = {}
        self.next_port = 1024
        self.ip_id = 0
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', 0))
        self.sock.settimeout(0.2)
        self.stopping = False
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def netdev(self, netdev_id):
        """The -netdev value connecting a NIC to this server."""
        return f'socket,id={netdev_id},udp=127.0.0.1:{self.sock.getsockname()[1]},localaddr=127.0.0.1:0'

    def stop(self):
        self.stopping = True
        self.thread.join()
        self.sock.close()

    def _run(self):
        while not self.stopping:
            try:
                frame, self.peer = self.sock.recvfrom(65536)
            except socket.timeout:
                continue
            try:
                self._frame(frame)
            except Exception as e:
                self.errors.append(f'{type(e).__name__}: {e}')

    # Link and network layers

    def _frame(self, frame):
        src, kind = frame[6:12], struct.unpack('!H', frame[12:14])[0]
        if kind == ETH_ARP:
            oper, sha, spa, tpa = struct.unpack('!H', frame[20:22])[0], frame[22:28], frame[28:32], frame[38:42]
            if oper == 1 and tpa == ip_bytes(SERVER):
                self._send_eth(sha, ETH_ARP, struct.pack('!HHBBH', 1, ETH_IP, 6, 4, 2) + SERVER_MAC + tpa + sha + spa)
        elif kind == ETH_IP:
            p = frame[14:]
            ihl, total = (p[0] & 15) * 4, struct.unpack('!H', p[2:4])[0]
            if p[9] != 17:
                return
            udp = p[ihl:total]
            sport, dport, ulen = struct.unpack('!HHH', udp[:6])
            payload = udp[8:ulen]
            self.client_mac = src
            if dport == 67:
                self._dhcp(payload)
            elif p[16:20] == ip_bytes(SERVER):
                if dport == 69:
                    self._read_request(sport, payload)
                elif dport in self.transfers:
                    self._transfer_packet(dport, payload)

    def _send_eth(self, dst, kind, payload):
        frame = dst + SERVER_MAC + struct.pack('!H', kind) + payload
        self.sock.sendto(frame.ljust(60, b'\0'), self.peer)

    def _send_udp(self, sport, dport, payload, dst=CLIENT, dst_mac=None):
        udp = struct.pack('!HHHH', sport, dport, 8 + len(payload), 0) + payload
        self.ip_id = (self.ip_id + 1) & 0xffff
        hdr = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(udp), self.ip_id, 0, 64, 17, 0,
                          ip_bytes(SERVER), ip_bytes(dst))
        hdr = hdr[:10] + struct.pack('!H', checksum(hdr)) + hdr[12:]
        self._send_eth(dst_mac or self.client_mac, ETH_IP, hdr + udp)

    # DHCP: an offer and an acknowledgement, broadcast, as any client takes.

    def _dhcp(self, p):
        if len(p) < 240 or p[0] != 1 or p[236:240] != DHCP_MAGIC:
            return
        kind = dhcp_options(p[240:]).get(53, b'\0')[0]
        reply = {DHCPDISCOVER: DHCPOFFER, DHCPREQUEST: DHCPACK}.get(kind)
        if reply is None:
            return
        bootp = (struct.pack('!BBBB', 2, 1, 6, 0) + p[4:8] + b'\0\0' + p[10:12] + b'\0' * 4
                 + ip_bytes(CLIENT) + ip_bytes(SERVER) + b'\0' * 4 + p[28:44] + b'\0' * 64
                 + self.bootfile.encode().ljust(128, b'\0'))
        options = (DHCP_MAGIC + bytes([53, 1, reply]) + bytes([54, 4]) + ip_bytes(SERVER)
                   + bytes([51, 4]) + struct.pack('!I', 86400) + bytes([1, 4]) + ip_bytes(NETMASK))
        if self.bootfile_size:
            size = os.path.getsize(os.path.join(self.root, self.bootfile))
            options += bytes([13, 2]) + struct.pack('!H', (size + 511) // 512)
        options += b'\xff'
        self._send_udp(67, 68, bootp + options, dst='255.255.255.255', dst_mac=BROADCAST_MAC)

    # TFTP

    def _read_request(self, cport, p):
        if struct.unpack('!H', p[:2])[0] != TFTP_RRQ:
            return
        fields = p[2:].split(b'\0')
        name = fields[0].decode()
        requested = {fields[i].decode().lower(): fields[i + 1].decode() for i in range(2, len(fields) - 1, 2)}
        self.requests.append((name, requested))
        self.next_port += 1
        sport = self.next_port
        path = os.path.join(self.root, name.lstrip('/'))
        if '..' in name.split('/') or not os.path.isfile(path):
            self._send_udp(sport, cport, struct.pack('!HH', TFTP_ERROR, 1) + b'File not found\0')
            return
        with open(path, 'rb') as f:
            data = f.read()
        accepted, blksize = {}, 512
        if 'blksize' in requested and 'blksize' in self.options:
            blksize = max(8, min(int(requested['blksize']), MAX_BLKSIZE))
            accepted['blksize'] = blksize
        if 'tsize' in requested and 'tsize' in self.options:
            accepted['tsize'] = len(data)
        self.transfers[sport] = {'client': cport, 'data': data, 'blksize': blksize, 'sent': 0}
        if accepted:
            self._send_udp(sport, cport, struct.pack('!H', TFTP_OACK)
                           + b''.join(f'{k}\0{v}\0'.encode() for k, v in accepted.items()))
        else:
            self._send_block(sport, 1)

    def _send_block(self, sport, n):
        xfer = self.transfers[sport]
        chunk = xfer['data'][(n - 1) * xfer['blksize']:n * xfer['blksize']]
        xfer['sent'] = n
        self._send_udp(sport, xfer['client'], struct.pack('!HH', TFTP_DATA, n & 0xffff) + chunk)

    def _transfer_packet(self, sport, p):
        op, block = struct.unpack('!HH', p[:4])
        xfer = self.transfers[sport]
        if op == TFTP_ERROR:
            del self.transfers[sport]
        elif op == TFTP_ACK and block == xfer['sent'] & 0xffff:
            if xfer['sent'] * xfer['blksize'] > len(xfer['data']):
                del self.transfers[sport]
            else:
                self._send_block(sport, xfer['sent'] + 1)
