"""Pretend to be the wand: sends the board's live UDP packets and posts a cast for each spell in turn,
so the live page can be tried without hardware. Ctrl+C to stop.

Run (with server.py running):  python3 fake_wand.py                  (server on this computer, port 8050)
                               python3 fake_wand.py 127.0.0.1 8050   (other port)
"""
import json
import math
import random
import socket
import sys
import time
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8050
SPELLS = ["LUMOS", "NOX", "LEVIOSA", "EXPELLIARMUS", "ACCIO"]
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)


def shape(s):  # wand-tip path, same shapes as test/test_dtw.cpp (y grows downward)
    if s in (0, 1):
        side = 1 if s == 0 else -1
        return [(side * math.sin(2 * math.pi * i / 24), -math.cos(2 * math.pi * i / 24)) for i in range(25)]
    if s == 2:
        return [(-1 + 2 * i / 12, -0.35 * math.sin(math.pi * 2 * i / 12)) for i in range(13)] + [(1.15, 0.9)]
    return [(-1, -1), (1, -1), (-1, 1), (1, 1)] if s == 3 else [(-1, -1), (0, 1), (1, -1)]


def along(p, u):  # point at fraction u of the path's length
    lens = [math.dist(p[i], p[i + 1]) for i in range(len(p) - 1)]
    target = u * sum(lens)
    for (a, b), seg in zip(zip(p, p[1:]), lens):
        if target <= seg or b is p[-1]:
            f = min(1, target / seg) if seg else 0
            return a[0] + f * (b[0] - a[0]), a[1] + f * (b[1] - a[1])
        target -= seg


def noise(n):
    return [[round(random.gauss(0, 2)) for _ in range(3)] for _ in range(n)]


def cast(s):  # gyro (deg/s) at 100 Hz: sideways tip speed = yaw (z), up-down = pitch (y), like the simulator
    n = random.randint(70, 120)
    pts = [along(shape(s), i / n) for i in range(n + 1)]
    clamp = lambda v: max(-2000, min(2000, round(v)))
    return [[round(random.gauss(0, 5)), clamp(-(b[1] - a[1]) * 6000), clamp(-(b[0] - a[0]) * 6000)]
            for a, b in zip(pts, pts[1:])]


def send(samples, rec):  # 8 samples every 80 ms, the same packet the board sends
    for i in range(0, len(samples), 8):
        g = [v for sample in samples[i:i + 8] for v in sample]
        packet = {"rec": rec, "teach": -1, "taught": 0, "light": 0, "g": g}
        sock.sendto(json.dumps(packet).encode(), (HOST, PORT))
        time.sleep(0.08)


def post(spell, score, d):
    body = json.dumps({"spell": spell, "score": score, "d": d}).encode()
    req = urllib.request.Request(f"http://{HOST}:{PORT}/api/cast", body, {"Content-Type": "application/json"})
    urllib.request.urlopen(req, timeout=3).read()


if __name__ == "__main__":
    print(f"Fake wand -> {HOST}:{PORT}. Ctrl+C to stop.")
    while True:
        for s, name in enumerate(SPELLS):
            send(noise(100), 0)
            send(cast(s) + noise(20), 1)
            d = [random.randint(150, 400) if i == s else random.randint(650, 1300) for i in range(len(SPELLS))]
            post(name, max(0, min(100, round(100 * (650 - d[s]) / 400))), d)
            print("cast", name)
