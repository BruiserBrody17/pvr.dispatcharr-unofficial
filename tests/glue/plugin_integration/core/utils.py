# Stand-in for Dispatcharr's core.utils.RedisClient: a tiny thread-safe RESP client (redis-py-like API subset).
import os
import socket
import threading


class ResponseError(Exception):
    pass


class _Client:
    def __init__(self, port):
        self.port = port
        self.local = threading.local()

    def _sock(self):
        s = getattr(self.local, "s", None)
        if s is None:
            s = socket.create_connection(("127.0.0.1", self.port))
            self.local.s = s
            self.local.f = s.makefile("rb")
        return s, self.local.f

    def _cmd(self, *args):
        s, f = self._sock()
        parts = [b"*%d\r\n" % len(args)]
        for x in args:
            b = x if isinstance(x, bytes) else str(x).encode()
            parts.append(b"$%d\r\n%s\r\n" % (len(b), b))
        s.sendall(b"".join(parts))
        return self._read(f)

    def _read(self, f):
        line = f.readline()
        t = line[:1]
        body = line[1:-2]
        if t == b"+":
            return body.decode() == "OK" or body.decode()
        if t == b"-":
            raise ResponseError(body.decode())
        if t == b":":
            return int(body)
        if t == b"$":
            n = int(body)
            if n < 0:
                return None
            d = f.read(n + 2)[:-2]
            return d
        if t == b"*":
            return [self._read(f) for _ in range(int(body))]
        raise RuntimeError("bad reply %r" % line)

    def get(self, k):
        return self._cmd("GET", k)

    def set(self, k, v, ex=None, nx=False, px=None, xx=False):
        a = ["SET", k, v]
        if ex is not None:
            a += ["EX", int(ex)]
        if px is not None:
            a += ["PX", int(px)]
        if nx:
            a.append("NX")
        if xx:
            a.append("XX")
        r = self._cmd(*a)
        return True if r is True else None

    def delete(self, *ks):
        return self._cmd("DEL", *ks)

    def exists(self, *ks):
        return self._cmd("EXISTS", *ks)

    def expire(self, k, s):
        return bool(self._cmd("EXPIRE", k, int(s)))

    def ttl(self, k):
        return self._cmd("TTL", k)

    def keys(self, pat="*"):
        return self._cmd("KEYS", pat)

    def scan_iter(self, match=None, count=None):
        # The stand-in server answers a whole SCAN in one reply (cursor 0), as redis-py's scan_iter would loop.
        reply = self._cmd("SCAN", 0, "MATCH", match or "*", "COUNT", count or 10)
        yield from reply[1]

    def time(self):
        seconds, microseconds = self._cmd("TIME")
        return int(seconds), int(microseconds)

    def eval(self, script, numkeys, *args):
        return self._cmd("EVAL", script, numkeys, *args)


class RedisClient:
    _c = None

    @classmethod
    def get_client(cls):
        if cls._c is None:
            cls._c = _Client(int(os.environ["FAKE_REDIS_PORT"]))
        return cls._c
