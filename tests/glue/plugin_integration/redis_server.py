# Minimal RESP2 Redis server: GET SET(EX/PX/NX/XX) DEL KEYS SCAN EXPIRE EXISTS TTL PING EVAL (the plugin's two scripts).
import socketserver
import sys
import threading
import time
import fnmatch

DATA = {}
EXP = {}
L = threading.Lock()
STATS = {}


def alive(k):
    e = EXP.get(k)
    if e is not None and e <= time.time():
        DATA.pop(k, None)
        EXP.pop(k, None)
    return k in DATA


def bulk(v):
    return b"$-1\r\n" if v is None else b"$%d\r\n%s\r\n" % (len(v), v)


def do_set(a):
    k, v = a[1], a[2]
    ex = None
    nx = xx = False
    i = 3
    while i < len(a):
        o = a[i].upper()
        if o == b"EX":
            ex = int(a[i + 1])
            i += 2
        elif o == b"PX":
            ex = int(a[i + 1]) / 1000.0
            i += 2
        elif o == b"NX":
            nx = True
            i += 1
        elif o == b"XX":
            xx = True
            i += 1
        else:
            return b"-ERR syntax\r\n"
    ex_ = alive(k)
    if nx and ex_:
        return b"$-1\r\n"
    if xx and not ex_:
        return b"$-1\r\n"
    DATA[k] = v
    EXP.pop(k, None)
    if ex is not None:
        EXP[k] = time.time() + ex
    return b"+OK\r\n"


def cmd(a):
    c = a[0].upper()
    STATS[c] = STATS.get(c, 0) + 1
    with L:
        if c == b"PING":
            return b"+PONG\r\n"
        if c == b"TIME":
            now = time.time()
            sec = int(now)
            return b"*2\r\n" + bulk(b"%d" % sec) + bulk(b"%d" % int((now - sec) * 1_000_000))
        if c == b"GET":
            return bulk(DATA[a[1]] if alive(a[1]) else None)
        if c == b"SET":
            return do_set(a)
        if c == b"DEL":
            n = 0
            for k in a[1:]:
                if alive(k):
                    DATA.pop(k)
                    EXP.pop(k, None)
                    n += 1
            return b":%d\r\n" % n
        if c == b"EXISTS":
            return b":%d\r\n" % sum(1 for k in a[1:] if alive(k))
        if c == b"EXPIRE":
            if not alive(a[1]):
                return b":0\r\n"
            EXP[a[1]] = time.time() + int(a[2])
            return b":1\r\n"
        if c == b"TTL":
            if not alive(a[1]):
                return b":-2\r\n"
            return b":%d\r\n" % (int(EXP[a[1]] - time.time()) if a[1] in EXP else -1)
        if c == b"KEYS":
            ks = [k for k in list(DATA) if alive(k) and fnmatch.fnmatchcase(k.decode(), a[1].decode())]
            return b"*%d\r\n" % len(ks) + b"".join(bulk(k) for k in ks)
        if c == b"SCAN":
            # Everything in one reply (cursor 0): MATCH is honoured, COUNT is only a hint.
            pattern = "*"
            for i, opt in enumerate(a[2:], start=2):
                if opt.upper() == b"MATCH" and i + 1 < len(a):
                    pattern = a[i + 1].decode()
            ks = [k for k in list(DATA) if alive(k) and fnmatch.fnmatchcase(k.decode(), pattern)]
            return b"*2\r\n" + bulk(b"0") + b"*%d\r\n" % len(ks) + b"".join(bulk(k) for k in ks)
        if c == b"EVAL":
            s = a[1].decode()
            keys = a[3 : 3 + int(a[2])]
            argv = a[3 + int(a[2]) :]
            cur = DATA[keys[0]] if alive(keys[0]) else None
            if "'SET', KEYS[1], ARGV[2], 'EX'" in s:  # CAS
                if cur == argv[0]:
                    DATA[keys[0]] = argv[1]
                    EXP[keys[0]] = time.time() + int(argv[2])
                    return b":1\r\n"
                return b":0\r\n"
            if "return redis.call('DEL', KEYS[1])" in s:  # release lock
                if cur == argv[0]:
                    DATA.pop(keys[0])
                    EXP.pop(keys[0], None)
                    return b":1\r\n"
                return b":0\r\n"
            return b"-ERR unknown script\r\n"
        if c == b"__STATS":
            return bulk(repr(STATS).encode())
        return b"-ERR unknown command %s\r\n" % c


class H(socketserver.StreamRequestHandler):
    def handle(self):
        while True:
            line = self.rfile.readline()
            if not line:
                return
            assert line[:1] == b"*", line
            n = int(line[1:])
            a = []
            for _ in range(n):
                n_bytes = int(self.rfile.readline()[1:])
                a.append(self.rfile.read(n_bytes + 2)[:-2])
            self.wfile.write(cmd(a))
            self.wfile.flush()


class S(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 256


S(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
