"""MirRoar - minimal desktop test client: handshake, dump the H.264 stream, print stats.
Usage: python tools/test_client.py [host] [seconds] [out.h264]
"""
import socket, struct, sys, time

host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 5
out = sys.argv[3] if len(sys.argv) > 3 else "test.h264"

s = socket.create_connection((host, 47800))
mask = int(sys.argv[4]) if len(sys.argv) > 4 else 3
hello = struct.pack("<IIIII64sI", 3, 0, 0, 0, 0, b"test-client", mask)
s.sendall(struct.pack("<IHH", len(hello), 100, 0) + hello)

def recv_all(n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError
        b += c
    return b

f = open(out, "wb")
t0 = time.time()
frames = keys = vbytes = abytes = 0
per = {}
files = {}
while time.time() - t0 < secs:
    size, typ, _ = struct.unpack("<IHH", recv_all(8))
    p = recv_all(size)
    if typ == 1:
        print("S_HELLO", struct.unpack("<I", p[:4])[0], p[4:68].split(b"\0")[0].decode())
    elif typ == 2:
        print("S_VIDEO_CONFIG stream=%d codec=%d %dx%d fps=%d src=%dx%d" % struct.unpack("<IIIIIII", p))
    elif typ == 3:
        sid, flags, ts, conv, enc, queue = struct.unpack("<IIQIII", p[:28])
        frames += 1
        keys += flags & 1
        vbytes += len(p) - 28
        per[sid] = per.get(sid, 0) + 1
        if sid not in files:
            files[sid] = open(out.replace(".h264", "_%d.h264" % sid), "wb")
        files[sid].write(p[28:])
    elif typ == 9:
        n = struct.unpack("<I", p[:4])[0]
        for i in range(n):
            print("monitor", i, struct.unpack("<iiIIB3x", p[4 + i * 20:4 + (i + 1) * 20]))
    elif typ == 4:
        pass
    elif typ == 5:
        print("S_CURSOR_SHAPE %dx%d hot=%d,%d" % struct.unpack("<HHHH", p[:8]))
    elif typ == 6:
        print("S_AUDIO_CONFIG rate=%d ch=%d bits=%d" % struct.unpack("<IHH", p))
    elif typ == 7:
        abytes += len(p)
el = time.time() - t0
for fh in files.values():
    fh.close()
print("per-stream", per)
print("frames=%d keys=%d video=%.1f Mbps audio=%.0f kB/s" % (frames, keys, vbytes * 8 / el / 1e6, abytes / el / 1000))
