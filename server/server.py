"""Spellcast server: live wand view, cast results and a leaderboard. Python 3 standard library only.

Run:  python3 server.py        (or python3 server.py 8050 to use another port)
then open the printed address on any device on the same WiFi.
The board streams its motion here over UDP and posts each cast over HTTP, both on PORT.
The page gets everything live from /api/live (Server-Sent Events).
What the laptop itself does for each spell (sounds, links, ...) lives in spells.py.
The wand's training is backed up to training.json (and backups/), and can be loaded back onto the wand.
In the wand's mouse mode, its movement drives the laptop's real mouse pointer (mouse.py).
"""
import importlib
import json
import math
import socket
import sys
import threading
import time
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import mouse

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8050
HERE = Path(__file__).parent
LOG = HERE / "casts.jsonl"  # one cast per line, so the leaderboard survives a restart
PAGE = HERE.parent / "sim" / "index.html"
SPELLS = {"LUMOS", "NOX", "LEVIOSA", "EXPELLIARMUS", "ACCIO", "FIZZLE"}
NAMES = ["LUMOS", "NOX", "LEVIOSA", "EXPELLIARMUS", "ACCIO"]  # spell numbers on the wand
TRAINING = HERE / "training.json"  # the wand's trained examples; "Load onto wand" sends this file
BACKUPS = HERE / "backups"  # every backup also lands here with a date, so none is ever overwritten

casts = [json.loads(line) for line in LOG.read_text().splitlines() if line] if LOG.exists() else []
player = {"name": "anon"}  # whoever is holding the wand, set from the page
events = []  # (id, json) of recent live/cast events, newest last
last_id = 0
changed = threading.Condition()
wand = {"addr": None, "seen": 0.0}  # where the wand's live packets come from, to send it commands

# Mouse mode feel. Tune here and restart the server; the wand doesn't need a new upload.
MOUSE_GAIN = 25    # pointer pixels per degree the wand tip turns
MOUSE_DEAD = 5     # deg/s: slower than this is hand shake, so the pointer stays put
MOUSE_ACCEL = 300  # deg/s: at this speed each degree moves the pointer twice as far
pointer = {"carry": [0.0, 0.0], "ok": None}
udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)


spells = None  # spells.py, loaded on the first cast
spell_lock = threading.Lock()  # one spell function at a time


def run_spell(cast):
    """Call spells.py's function for this spell. The file is reloaded every time, so edits apply without a restart,
    and a mistake in it only prints an error."""
    global spells
    with spell_lock:
        try:
            spells = importlib.reload(spells) if spells else importlib.import_module("spells")
            handler = getattr(spells, cast["spell"].lower(), None)
            if handler:
                handler(dict(cast))
        except Exception:
            print(f"spells.py failed on {cast['spell']}:")
            traceback.print_exc()


def load_spells():
    """spells.py, freshly reloaded (it's edited while the server runs)."""
    global spells
    with spell_lock:
        spells = importlib.reload(spells) if spells else importlib.import_module("spells")
        return spells


def spell_links():
    """{spell: URL it opens} for every spell in spells.py's LINKS, with the web page's changes applied."""
    sp = load_spells()
    return {name: sp.link(name) for name in sp.LINKS}


def set_spell_link(name, url):
    """Save the URL a spell opens (from the web page). An empty URL goes back to spells.py's default."""
    sp = load_spells()
    if name not in sp.LINKS:
        raise ValueError(f"{name} doesn't open a link (add it to LINKS in spells.py)")
    url = str(url).strip()
    if url and (not url.startswith(("http://", "https://")) or len(url) > 2000 or any(c.isspace() for c in url)):
        raise ValueError("use a web address starting with http:// or https://")  # nothing that opens files or apps
    try:
        saved = json.loads(sp.LINKS_FILE.read_text())
    except (OSError, ValueError):
        saved = {}
    if url:
        saved[name] = url
    else:
        saved.pop(name, None)
    sp.LINKS_FILE.write_text(json.dumps(saved, indent=2) + "\n")
    return spell_links()


def publish(event):
    global last_id
    with changed:
        last_id += 1
        events.append((last_id, json.dumps(event)))
        del events[:-200]
        changed.notify_all()


def record_cast(body, src):
    """A cast from the wand (UDP) or the web simulator (HTTP): log it, show it live, run its spells.py function."""
    spell = str(body["spell"]).upper()
    if spell not in SPELLS:
        raise ValueError(f"unknown spell {spell}")
    d = body.get("d", [])  # wand: distance per spell in thousandths, -1 = not taught
    if not isinstance(d, list) or len(d) > 10 or not all(isinstance(v, int) and -1 <= v <= 100000 for v in d):
        raise ValueError("d must be up to 10 whole numbers")
    c = {"player": player["name"], "spell": spell, "score": max(0, min(100, int(body["score"]))), "src": src,
         "t": int(time.time())}
    casts.append(c)
    with LOG.open("a") as f:
        f.write(json.dumps(c) + "\n")
    publish({"type": "cast", **c, "d": d})
    threading.Thread(target=run_spell, args=(c,), daemon=True).start()  # don't hold anything up
    return c


def leaderboard():
    stats = {}
    for c in casts:
        if c["spell"] == "FIZZLE":
            continue
        s = stats.setdefault(c["player"], {"player": c["player"], "casts": 0, "total": 0, "best": 0})
        s["casts"] += 1
        s["total"] += c["score"]
        s["best"] = max(s["best"], c["score"])
    rows = [{"player": s["player"], "casts": s["casts"], "best": s["best"], "avg": round(s["total"] / s["casts"])}
            for s in stats.values()]
    return sorted(rows, key=lambda r: (-r["avg"], -r["casts"]))


def wand_packet(data):
    """One UDP packet from the board -> a live event or a mouse click, or None if it isn't valid."""
    try:
        p = json.loads(data)
        if "cast" in p:  # the wand's result, sent twice; "id" lets the repeat be dropped
            return {"type": "wandcast", "id": int(p["id"]), "body": {"spell": p["cast"], "score": p["score"], "d": p["d"]}}
        if "click" in p:
            return {"type": "click", "click": p["click"]} if p["click"] in ("left", "right", "double") else None
        g = p["g"]
        if not isinstance(g, list) or len(g) > 60 or len(g) % 3 or not all(
                isinstance(v, int) and -2000 <= v <= 2000 for v in g):
            return None
        return {"type": "live", "rec": int(p["rec"]) & 1, "teach": max(-1, min(9, int(p["teach"]))),
                "taught": max(0, min(9, int(p["taught"]))), "light": int(p["light"]) & 1,
                "lost": int(p.get("lost", 0)) & 1, "mouse": int(p.get("mouse", 0)) & 1, "g": g}
    except (ValueError, KeyError, TypeError):
        return None


def listen_udp():
    udp.bind(("0.0.0.0", PORT))
    while True:
        data, addr = udp.recvfrom(2048)
        event = wand_packet(data)
        if event:
            wand["addr"], wand["seen"] = addr, time.time()
            if event["type"] == "wandcast":
                if event["id"] != wand.get("last_cast"):
                    wand["last_cast"] = event["id"]
                    try:
                        record_cast(event["body"], "wand")
                    except (ValueError, KeyError, TypeError) as e:
                        print("Bad cast from the wand:", e)
                continue
            if event["type"] == "click":
                if mouse_ready():
                    mouse.click(event["click"])
            elif event["mouse"]:
                move_pointer(event["g"])
            publish(event)


def mouse_ready():  # asked once: macOS only applies a new Accessibility permission after a restart anyway
    if pointer["ok"] is None:
        pointer["ok"] = mouse.allowed()
        if not pointer["ok"]:
            print("Mouse mode: macOS is blocking pointer control. Allow the app running this server (e.g. Terminal) in "
                  "System Settings > Privacy & Security > Accessibility, then restart the server.")
    return pointer["ok"]


def move_pointer(g):
    """Wand samples (10 ms apart, wand axes in deg/s) -> pointer movement. Tip left/right = x, tip up/down = y."""
    if not mouse_ready():
        return
    carry = pointer["carry"]
    for i in range(0, len(g), 3):
        pitch, yaw = g[i + 1], g[i + 2]
        speed = math.hypot(pitch, yaw)
        if speed < MOUSE_DEAD:
            continue
        px_per_dps = (speed - MOUSE_DEAD) / speed * (1 + speed / MOUSE_ACCEL) * MOUSE_GAIN / 100  # 100 samples/s
        carry[0] -= yaw * px_per_dps    # yaw + = tip left -> pointer left
        carry[1] -= pitch * px_per_dps  # pitch + = tip up -> pointer up (screen y grows downward)
    dx, dy = int(carry[0]), int(carry[1])  # whole pixels now, keep the fraction for next time
    if dx or dy:
        carry[0] -= dx
        carry[1] -= dy
        mouse.move(dx, dy)


def tell_wand(command):  # EXPORT or IMPORT, sent back to where the wand's packets come from
    if not wand["addr"] or time.time() - wand["seen"] > 5:
        raise ValueError("the wand isn't connected: no live data from it in the last 5 s")
    udp.sendto(command.encode(), wand["addr"])


def save_training(body):
    """The wand's upload -> training.json + a dated copy in backups/. Returns how many examples."""
    fmt, items = int(body["format"]), body["templates"]
    if not isinstance(items, list) or not 0 < len(items) <= 15:
        raise ValueError("templates must be a list of 1-15 examples")
    clean = []
    for t in items:
        slot, spell, v = int(t["slot"]), int(t["spell"]), t["v"]
        if not (0 <= slot < 15 and 0 <= spell < len(NAMES)) or not isinstance(v, list) or len(v) != 96 or not all(
                isinstance(x, (int, float)) and math.isfinite(x) and abs(x) < 1000 for x in v):
            raise ValueError(f"example in slot {slot} isn't valid")
        clean.append({"slot": slot, "spell": spell, "name": NAMES[spell], "v": [round(float(x), 4) for x in v]})
    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    text = ('{"format": %d, "saved": "%s", "templates": [\n%s\n]}\n'
            % (fmt, stamp, ",\n".join(json.dumps(t) for t in clean)))  # one example per line
    BACKUPS.mkdir(exist_ok=True)
    (BACKUPS / f"training-{time.strftime('%Y%m%d-%H%M%S')}.json").write_text(text)
    TRAINING.write_text(text)
    return len(clean)


def training_info():
    try:
        t = json.loads(TRAINING.read_text())
        return {"saved": t.get("saved", ""), "count": len(t["templates"]),
                "spells": sorted({x["name"] for x in t["templates"]}, key=NAMES.index)}
    except (OSError, ValueError, KeyError):
        return None


def training_txt():
    """training.json as plain numbers, easy for the board to read: format count / slot spell 96 numbers ... / count."""
    t = json.loads(TRAINING.read_text())
    lines = [f"{t['format']} {len(t['templates'])}"]
    lines += [f"{x['slot']} {x['spell']} " + " ".join(f"{v:.4f}" for v in x["v"]) for x in t["templates"]]
    lines.append(str(len(t["templates"])))  # end marker: the board checks it to know nothing was cut off
    return "\n".join(lines) + "\n"


class Handler(BaseHTTPRequestHandler):
    def send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/api/state":
            return self.send(200, {"player": player["name"], "feed": casts[-12:][::-1], "leaderboard": leaderboard(),
                                   "training": training_info(), "wand": time.time() - wand["seen"] < 5})
        if self.path == "/api/links":
            try:
                return self.send(200, spell_links())
            except Exception as e:  # e.g. a mistake in spells.py
                return self.send(500, {"error": f"spells.py: {e}"})
        if self.path in ("/api/training", "/api/training.txt"):
            if not TRAINING.exists():
                return self.send(404, {"error": "no training saved yet"})
            if self.path.endswith(".txt"):
                return self.send(200, training_txt().encode(), "text/plain")
            return self.send(200, TRAINING.read_bytes())
        if self.path == "/api/live":
            return self.stream()
        if self.path in ("/", "/index.html"):
            return self.send(200, PAGE.read_bytes(), "text/html; charset=utf-8")
        self.send(404, {"error": "not found"})

    def stream(self):  # Server-Sent Events: every wand packet and cast, as it happens
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        seen = last_id
        try:
            while True:
                with changed:
                    changed.wait_for(lambda: last_id > seen, timeout=15)
                    new = [e for e in events if e[0] > seen]
                if new:
                    seen = new[-1][0]
                    self.wfile.write("".join(f"data: {e}\n\n" for _, e in new).encode())
                else:
                    self.wfile.write(b": still here\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            return

    def do_POST(self):
        try:
            n = int(self.headers.get("Content-Length", 0))
            if n > (65536 if self.path == "/api/training" else 2048):
                raise ValueError("body too large")
            body = json.loads(self.rfile.read(n) or b"{}")
            if self.path == "/api/cast":
                return self.send(200, record_cast(body, "sim" if body.get("src") == "sim" else "wand"))
            if self.path == "/api/training":  # the wand uploading its training
                count = save_training(body)
                publish({"type": "training", **training_info()})
                print(f"Training saved: {count} examples -> {TRAINING.name}")
                return self.send(200, {"saved": count})
            if self.path == "/api/training/export":  # web button: ask the wand to upload
                tell_wand("EXPORT")
                return self.send(200, {"ok": True})
            if self.path == "/api/training/import":  # web button: ask the wand to download training.json
                if not TRAINING.exists():
                    raise ValueError("no training saved on this laptop yet")
                tell_wand("IMPORT")
                return self.send(200, {"ok": True})
            if self.path == "/api/links":
                return self.send(200, set_spell_link(str(body["spell"]).lower(), body.get("url", "")))
            if self.path == "/api/player":
                player["name"] = str(body["name"]).strip()[:20] or "anon"
                return self.send(200, player)
        except (ValueError, KeyError, TypeError) as e:
            return self.send(400, {"error": str(e)})
        self.send(404, {"error": "not found"})

    def log_message(self, fmt, *args):  # keep the terminal for casts only
        if self.command == "POST":
            print(self.address_string(), fmt % args)


def lan_ip():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect(("10.255.255.255", 1))  # no packet is sent; this just picks the outgoing interface
            return s.getsockname()[0]
        except OSError:
            return "127.0.0.1"


if __name__ == "__main__":
    threading.Thread(target=listen_udp, daemon=True).start()
    print(f"Spellcast server: http://{lan_ip()}:{PORT}  (put this IP in SERVER_IP and {PORT} in SERVER_PORT in spellcast.ino)")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
