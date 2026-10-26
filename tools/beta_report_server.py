#!/usr/bin/env python3
"""Problem reports and the sign-up queue of the Patapon 3 online beta (stdlib only).

One HTTP service behind the Cloudflare Tunnel, told apart by the Host header:
- `report.montick.app` (and any other host): `GET /` is the Spanish report form, `POST /report` takes a report
  from that form or from the launcher's "Reportar problema" button (with the tail of the game log);
- `beta.montick.app` (Host starting with `beta.`): `GET /` is the sign-up page (email + consent), `POST /cola`
  stores the email in the queue the user draws invitations from (tools/beta_cola.py). Nothing is sent to anyone.
  `GET /salas` is the public lobby page (players online, rooms) that polls `GET /salas.json`, the summary the
  infrastructure server writes with `--status` (online ids and game settings only, never addresses).

Both write into the beta SQLite file the infrastructure server keeps its metrics in (tables `reports` and
`queue`). A report names its invite code by the code's label from the server's beta config, never the code.

    python -I tools/beta_report_server.py [--host 127.0.0.1] [--port 27322] [--db beta.db] [--beta beta_config.json]
        [--status salas.json]
    python -I tools/beta_report_server.py --selftest
"""
import argparse
import base64
import binascii
import collections
import hashlib
import http.server
import json
import os
import re
import sqlite3
import sys
import threading
import time
import zlib

MAX_BODY = 1024 * 1024       # a bigger request body is refused with 413
MAX_LOG = 640 * 1024         # decoded log text attached to a report
REPORTS_PER_HOUR = 10        # per address
SIGNUPS_PER_HOUR = 5         # per address
STATUS_STALE = 15            # seconds: an older lobby summary means the infrastructure server is not running
MODES = ("escondite", "coop", "vs", "otro")
EMAIL = re.compile(r"[^@\s]+@[^@\s]+\.[^@\s]+")
SIGNUP_OK = "¡Listo! Si sales en el sorteo te llegará un correo con tu código."
CONSENT = "Acepto que guarden mi correo solo para esta beta y lo borren al terminar"

SCHEMA = """
CREATE TABLE IF NOT EXISTS reports(id INTEGER PRIMARY KEY, t_utc TEXT, source TEXT, label TEXT, nick TEXT,
    mode TEXT, when_text TEXT, what TEXT, version TEXT, machine TEXT, log_z BLOB);
CREATE TABLE IF NOT EXISTS queue(id INTEGER PRIMARY KEY, t_utc TEXT, email TEXT UNIQUE, invited_utc TEXT,
    label TEXT);
"""


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def utc_now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


# Same as tools/infra_server.py (that file runs with -I and is copied alone to the server machine, so it is not
# imported); tools/beta_cola.py --selftest checks the three give the same hash.
def normalize_code(text):
    return re.sub(r"[\s-]", "", str(text)).upper()


def code_hash(text):
    return hashlib.sha256(normalize_code(text).encode()).hexdigest()


def disable_quick_edit():
    """Windows: a click in the console window starts a text selection that blocks every write to it; turn QuickEdit
    off for this console. True when it was on."""
    if sys.platform != "win32":
        return False
    import ctypes
    k32 = ctypes.windll.kernel32
    h = k32.GetStdHandle(-10)  # STD_INPUT_HANDLE
    mode = ctypes.c_uint32()
    if not k32.GetConsoleMode(h, ctypes.byref(mode)) or not mode.value & 0x40:
        return False
    return bool(k32.SetConsoleMode(h, (mode.value & ~0x40) | 0x80))


class Labels:
    """Invite code hash -> label from the server's beta config, re-read when the file changes."""

    def __init__(self, path):
        self.path = path
        self.mtime = None
        self.by_hash = {}
        self.lock = threading.Lock()

    def label(self, code):
        if not self.path or not code:
            return None
        with self.lock:
            try:
                mtime = os.stat(self.path).st_mtime_ns
                if mtime != self.mtime:
                    with open(self.path, encoding="utf-8") as f:
                        cfg = json.load(f)
                    self.by_hash = {c["sha256"]: c["label"] for c in cfg.get("codes", [])}
                    self.mtime = mtime
            except (OSError, ValueError, KeyError, TypeError) as e:
                log("beta config not read (%s): keeping the previous one" % e)
            return self.by_hash.get(code_hash(code), "?")


class RateLimit:
    def __init__(self, per_hour):
        self.per_hour = per_hour
        self.seen = collections.defaultdict(collections.deque)
        self.lock = threading.Lock()

    def full(self, addr, now):
        with self.lock:
            q = self.seen[addr]
            while q and q[0] <= now - 3600:
                q.popleft()
            return len(q) >= self.per_hour

    def add(self, addr, now):
        with self.lock:
            self.seen[addr].append(now)


class Service(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, host, port, db, beta=None, clock=time.time, status=None):
        self.db = db
        self.status = status    # the infrastructure server's --status file (lobby summary)
        self.labels = Labels(beta)
        self.clock = clock
        self.reports = RateLimit(REPORTS_PER_HOUR)
        self.signups = RateLimit(SIGNUPS_PER_HOUR)
        with self.connect() as c:
            c.execute("PRAGMA journal_mode=WAL")
            c.executescript(SCHEMA)
        super().__init__((host, port), Handler)

    def connect(self):
        return sqlite3.connect(self.db, timeout=10)


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "p3beta"
    sys_version = ""

    def log_message(self, fmt, *args):
        pass

    def answer(self, status, body, ctype="application/json; charset=utf-8"):
        if not isinstance(body, bytes):
            body = (json.dumps(body, ensure_ascii=False) if not isinstance(body, str) else body).encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def addr(self):
        # Behind the tunnel every request comes from cloudflared on 127.0.0.1; Cloudflare names the client.
        return self.headers.get("CF-Connecting-IP") or self.client_address[0]

    def queue_host(self):
        return (self.headers.get("Host") or "").lower().startswith("beta.")

    def do_GET(self):
        path = self.path.split("?")[0]
        if self.queue_host() and path == "/salas":
            return self.answer(200, LOBBY_PAGE, "text/html; charset=utf-8")
        if self.queue_host() and path == "/salas.json":
            return self.answer(200, self.lobby())
        if path != "/":
            return self.answer(404, {"error": "not found"})
        page = SIGNUP_PAGE if self.queue_host() else REPORT_PAGE
        self.answer(200, page, "text/html; charset=utf-8")

    def lobby(self):
        """The lobby summary while the infrastructure server keeps it fresh; {"ok": false} otherwise."""
        try:
            with open(self.server.status, encoding="utf-8-sig") as f:
                snap = json.load(f)
            if abs(self.server.clock() - float(snap["t"])) <= STATUS_STALE:
                return dict(snap, ok=True)
        except (OSError, TypeError, ValueError, KeyError):
            pass
        return {"ok": False}

    def read_json(self):
        """The request body as a dict, or None after answering the error."""
        try:
            n = int(self.headers.get("Content-Length", ""))
        except ValueError:
            self.close_connection = True
            self.answer(411, {"error": "length required"})
            return None
        if n > MAX_BODY:
            self.close_connection = True
            if n <= 4 * MAX_BODY:  # drain it so the client reads the answer instead of a reset
                while n > 0:
                    chunk = self.rfile.read(min(n, 65536))
                    if not chunk:
                        break
                    n -= len(chunk)
            self.answer(413, {"error": "too large"})
            return None
        try:
            body = json.loads(self.rfile.read(n))
        except (ValueError, UnicodeDecodeError):
            body = None
        if not isinstance(body, dict):
            self.answer(400, {"error": "bad json"})
            return None
        return body

    def do_POST(self):
        path = self.path.split("?")[0]
        if path not in ("/report", "/cola"):
            self.close_connection = True
            return self.answer(404, {"error": "not found"})
        body = self.read_json()
        if body is None:
            return
        if path == "/report":
            self.report(body)
        else:
            self.signup(body)

    def report(self, b):
        def text(key, limit, required=False):
            v = b.get(key, "")
            if v is None:
                v = ""
            if not isinstance(v, str) or len(v) > limit or (required and not v.strip()):
                raise ValueError(key)
            return v

        try:
            if b.get("source") not in ("launcher", "web") or b.get("mode") not in MODES:
                raise ValueError("source/mode")
            what, when = text("what", 4000, True), text("when", 100)
            nick, code = text("nick", 32), text("code", 64)
            version, machine = text("version", 64), text("machine", 64)
            log_z = None
            raw = b.get("log")
            if raw:
                if not isinstance(raw, str) or len(raw) > (MAX_LOG * 4) // 3 + 8:
                    raise ValueError("log")
                data = base64.b64decode(raw, validate=True)
                if len(data) > MAX_LOG:
                    raise ValueError("log")
                log_z = zlib.compress(data.decode("utf-8", "replace").encode(), 6)
        except (ValueError, binascii.Error) as e:
            return self.answer(400, {"error": "bad field: %s" % e})
        addr, now = self.addr(), self.server.clock()
        if self.server.reports.full(addr, now):
            return self.answer(429, {"error": "too many reports, try later"})
        self.server.reports.add(addr, now)
        label = self.server.labels.label(code.strip()) if code.strip() else None
        with self.server.connect() as c:
            cur = c.execute("INSERT INTO reports(t_utc, source, label, nick, mode, when_text, what, version, machine,"
                            " log_z) VALUES (?,?,?,?,?,?,?,?,?,?)",
                            (utc_now(), b["source"], label, nick, b["mode"], when, what, version, machine, log_z))
            rid = cur.lastrowid
        log("report #%d from %s (%s, %s, log %s)" % (rid, label or "-", b["source"], b["mode"],
                                                      "yes" if log_z else "no"))
        self.answer(200, {"ok": True, "id": rid})

    def signup(self, b):
        email = b.get("email")
        if not isinstance(email, str):
            return self.answer(400, {"error": "bad email"})
        email = email.strip().lower()
        if len(email) > 254 or not EMAIL.fullmatch(email):
            return self.answer(400, {"error": "Revisa tu correo."})
        if b.get("consent") is not True:
            return self.answer(400, {"error": "Marca la casilla de consentimiento."})
        if b.get("website"):  # the hidden field: only bots fill it; they get the same answer
            return self.answer(200, {"ok": True, "message": SIGNUP_OK})
        addr, now = self.addr(), self.server.clock()
        if self.server.signups.full(addr, now):
            return self.answer(429, {"error": "Demasiados intentos; prueba en una hora."})
        self.server.signups.add(addr, now)
        with self.server.connect() as c:
            new = c.execute("INSERT OR IGNORE INTO queue(t_utc, email) VALUES (?, ?)", (utc_now(), email)).rowcount
            total = c.execute("SELECT COUNT(*) FROM queue").fetchone()[0]
        if new:
            log("queue: new sign-up (%d in the queue)" % total)
        self.answer(200, {"ok": True, "message": SIGNUP_OK})


PAGE_STYLE = """
:root{--bg:#f6f1e7;--fg:#1d1a16;--muted:#6b6258;--card:#fffdf8;--line:#d9cfbf;--accent:#b3261e;--ok:#1b6e3a}
@media (prefers-color-scheme:dark){:root{--bg:#16130f;--fg:#efe8dc;--muted:#a59a8c;--card:#211d18;--line:#3a332b;
--accent:#ff6b5e;--ok:#5fd18b}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 system-ui,sans-serif}
main{max-width:560px;margin:0 auto;padding:32px 16px}h1{font-size:1.6rem;margin:0 0 8px}
p{color:var(--muted);margin:0 0 16px}form{background:var(--card);border:1px solid var(--line);border-radius:12px;
padding:20px}label{display:block;font-weight:600;margin:14px 0 6px}
input[type=text],input[type=email],select,textarea{width:100%;padding:10px;border:1px solid var(--line);
border-radius:8px;background:var(--bg);color:var(--fg);font:inherit}textarea{min-height:140px;resize:vertical}
.check{display:flex;gap:10px;align-items:flex-start;font-weight:400;margin-top:16px}.check input{margin-top:5px}
button{margin-top:18px;width:100%;padding:12px;border:0;border-radius:8px;background:var(--accent);color:#fff;
font:600 1rem system-ui,sans-serif;cursor:pointer}button:disabled{opacity:.6}
#msg{margin-top:14px;font-weight:600}#msg.ok{color:var(--ok)}#msg.err{color:var(--accent)}
.hp{position:absolute;left:-10000px;width:1px;height:1px;overflow:hidden}
"""

# The public beta (no invite codes): what to download and how to start. Bump the zip name with every package.
DOWNLOAD_URL = "https://descargas.montick.app/patapon3-beta1-u5.zip"

SIGNUP_PAGE = """<!doctype html><html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Beta Patapon 3 online</title>
<style>%s
.dl{display:block;margin-top:18px;padding:14px;border-radius:8px;background:var(--accent);color:#fff;
text-align:center;font:600 1.05rem system-ui,sans-serif;text-decoration:none}ol{margin:0;padding-left:22px}
li{margin:6px 0}</style></head><body><main>
<h1>Beta del modo online de Patapon 3</h1>
<p>La beta del modo online del port nativo para PC está <strong>abierta a todos</strong>, sin código de invitación,
hasta el <strong>domingo 1 de noviembre a las 23:59</strong> (hora CDMX). Necesitas tu propia copia (ISO) de
Patapon 3.</p>
<p><strong>Qué ISO sirve:</strong> la versión europea (UCES01421) o la de Patapon 3 DxD. La versión americana
(UCUS98751) y la japonesa todavía no se pueden instalar.</p>
<form onsubmit="return false">
<ol>
<li>Descarga el zip y descomprímelo en una carpeta tuya (por ejemplo, Documentos).</li>
<li>Abre <code>launcher.exe</code> y elige tu ISO.</li>
<li>Escoge tu apodo: es tu nombre en las salas online.</li>
<li>Dale a Jugar. Si Windows pregunta por la red, acepta para jugar con gente de fuera de tu casa.</li>
</ol>
<a class="dl" href="%s">Descargar la beta</a>
</form>
<h2 style="font-size:1.15rem;margin:24px 0 8px">¿Windows o el antivirus lo bloquean?</h2>
<p>El juego es nuevo y no está firmado, así que Windows y algunos antivirus desconfían de él aunque no tenga nada
malo. Descárgalo solo de este enlace.</p>
<ol>
<li><strong>«Windows protegió su PC»:</strong> pulsa <em>Más información</em> y luego <em>Ejecutar de todas
formas</em>.</li>
<li><strong>Si el antivirus borró un archivo</strong> (<code>launcher.exe</code> o
<code>Patapon 3 DxD Remastered.exe</code>): abre <em>Seguridad de Windows → Protección contra virus y amenazas →
Historial de protección</em>, elige el aviso y pulsa <em>Acciones → Restaurar</em>.</li>
<li><strong>Para que no vuelva a pasar:</strong> en <em>Protección contra virus y amenazas → Administrar la
configuración → Exclusiones → Agregar exclusión → Carpeta</em>, elige tu carpeta <code>Patapon3Recomp</code>.
Con otro antivirus (Avast, AVG, Norton…) es igual: restáuralo de la cuarentena y añade la carpeta a sus
excepciones.</li>
<li><strong>Si falta algún archivo</strong>, vuelve a descomprimir el zip encima de tu carpeta: tus partidas no se
tocan.</li>
</ol>
<p style="margin-top:20px;text-align:center"><a href="/salas">¿Hay gente jugando? Mira las salas</a> ·
<a href="https://report.montick.app">Reportar un problema</a></p></main></body></html>
""" % (PAGE_STYLE, DOWNLOAD_URL)

LOBBY_PAGE = r"""<!doctype html>
<!--
THESIS: the lobby is a split-flap departures board; rooms are departures, counters are flap digits, and a change in the
world is a change of flaps. Refuses the dashboard of metric tiles and room cards.
OWN-WORLD: vermilion wall, ink-black flaps with a hinge line, ivory print, green/amber/blue state print, ivory
engraved plate, corner screws, one condensed DIN-style face in capitals.
STORY: a player sees at once how many are online and which rooms are open, searching or playing, then decides to launch.
FIRST VIEWPORT: wall + screwed board; plate "SALAS EN LINEA", counter strip (JUGADORES, SALAS, VS EN CURSO as flap
digits), then one row per room (tag, name, players, state, detail line).
FORM: departures board (grounded candidate 6 of 7, staging: counters strip over rows); seed e62684e0.
-->
<html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="dark">
<title>Salas de Patapon 3</title>
<style>
:root{
 --wall:#d23a1c;--wall-deep:#a82a12;
 --ink:#1a1511;--ink-lit:#2a231b;--hinge:#050403;
 --print:#f3e8c9;--print-dim:#b8ab8c;
 --open:#86e08f;--active:#ffb832;--search:#8ccbff;--plate:#efe3c2;
 --u:clamp(.95rem,4.6vw,1.5rem);
 --face:Bahnschrift,"DIN Alternate","Roboto Condensed","Arial Narrow",sans-serif;
}
*{box-sizing:border-box}
html{background:var(--wall-deep)}
body{margin:0;min-height:100vh;color:var(--print);font:600 1rem/1.3 var(--face);
 background:
  radial-gradient(120% 80% at 50% 0%,var(--wall) 0%,var(--wall-deep) 100%);
 padding:clamp(14px,4vw,40px) 16px 40px}
.wrap{max-width:880px;margin:0 auto}
.board{position:relative;background:var(--ink);border-radius:10px;
 padding:clamp(18px,3.4vw,28px) clamp(12px,3vw,24px) clamp(14px,2.4vw,20px);
 box-shadow:0 2px 0 #ffffff22 inset,0 0 0 clamp(5px,1.2vw,9px) #2b1a14,0 14px 30px #00000070}
.screw{position:absolute;width:11px;height:11px;border-radius:50%;
 background:radial-gradient(circle at 35% 30%,#8d8372,#3a342b 70%);box-shadow:0 1px 1px #000a}
.screw::after{content:"";position:absolute;left:2px;right:2px;top:5px;height:1px;background:#1a1611;transform:rotate(35deg)}
.screw.a{left:7px;top:7px}.screw.b{right:7px;top:7px}.screw.c{left:7px;bottom:7px}.screw.d{right:7px;bottom:7px}

.plate{display:inline-block;background:var(--plate);color:#1a1511;border-radius:2px;
 padding:.35em .9em;font-size:clamp(1rem,3.8vw,1.25rem);letter-spacing:.14em;text-transform:uppercase;
 box-shadow:0 1px 0 #fff8 inset,0 2px 3px #0008;margin:0 0 clamp(14px,3vw,22px)}
h1{margin:0;font:inherit;display:inline}

/* counters */
.counters{display:flex;flex-wrap:wrap;gap:14px clamp(18px,5vw,44px);align-items:flex-end;
 padding-bottom:clamp(14px,3vw,20px);border-bottom:2px solid #0a0806;box-shadow:0 1px 0 #ffffff12}
.counter{display:flex;flex-direction:column;gap:6px}
.cap{font-size:.78rem;letter-spacing:.14em;text-transform:uppercase;color:var(--print-dim)}
.counter .word{--u:clamp(1.7rem,9vw,2.9rem)}

/* flaps */
.word{display:inline-flex;gap:2px;--cw:calc(var(--u)*.78);--ch:calc(var(--u)*1.3)}
.f{position:relative;display:inline-block;width:var(--cw);height:var(--ch);border-radius:3px;
 background:linear-gradient(180deg,#14100c 0 49%,var(--ink-lit) 51% 100%);
 box-shadow:0 1px 0 #ffffff14 inset,0 1px 2px #000b;perspective:300px;overflow:hidden;
 font-size:var(--u);line-height:var(--ch);text-align:center;color:var(--print);letter-spacing:0}
.f::after{content:"";position:absolute;left:0;right:0;top:calc(50% - 1px);height:2px;background:var(--hinge);z-index:2}
.f i{position:absolute;inset:0;font-style:normal;display:block;transform-origin:50% 50%;backface-visibility:hidden}
.f i.flip{animation:flap .09s linear}
@keyframes flap{0%{transform:rotateX(0)}49.9%{transform:rotateX(-88deg)}50%{transform:rotateX(88deg)}100%{transform:rotateX(0)}}
.t-open .f{color:var(--open)}.t-active .f{color:var(--active)}.t-search .f{color:var(--search)}.t-dim .f{color:var(--print-dim)}

/* rows */
.rows{margin-top:clamp(10px,2.4vw,16px);display:flex;flex-direction:column}
.row{display:grid;align-items:center;gap:8px 14px;padding:clamp(9px,2vw,12px) 0;
 grid-template-columns:auto 1fr auto auto;
 grid-template-areas:"tag name players state" ". det det det";
 border-top:1px solid #ffffff12}
.row:first-child{border-top:0}
.tag{grid-area:tag;display:inline-block;min-width:3.6rem;text-align:center;font-size:.78rem;letter-spacing:.12em;
 text-transform:uppercase;padding:.28em .5em;border-radius:2px}
.tag.vs{background:var(--plate);color:#1a1511}
.tag.coop{border:1.5px solid var(--print-dim);color:var(--print)}
.n{grid-area:name}.p{grid-area:players}.s{grid-area:state}
.det{grid-area:det;font-size:.78rem;letter-spacing:.1em;text-transform:uppercase;color:var(--print-dim);min-height:0}
.det:empty{display:none}
.det b{color:var(--print);font-weight:700}

@media (max-width:620px){
 .row{grid-template-columns:auto 1fr;grid-template-areas:"tag name" "players state" "det det"}
 .p,.s{justify-self:start}
 .row{gap:8px 12px}
 .s{justify-self:end}
}

.empty .row{opacity:.9}
.foot{margin:18px 2px 0;display:flex;flex-wrap:wrap;justify-content:space-between;gap:8px 18px;
 font-size:.78rem;letter-spacing:.08em;text-transform:uppercase;color:#fff}
.foot a{color:#fff;text-underline-offset:3px}
.live::before{content:"";display:inline-block;width:.6em;height:.6em;border-radius:50%;margin-right:.55em;
 background:var(--open);box-shadow:0 0 0 2px #0003;animation:beat 2.4s ease-in-out infinite}
.live.off::before{background:#ffd0c4;animation:none}
@keyframes beat{50%{opacity:.35}}
.sr{position:absolute;width:1px;height:1px;overflow:hidden;clip:rect(0 0 0 0);white-space:nowrap}
:focus-visible{outline:3px solid #fff;outline-offset:3px}
@media (prefers-reduced-motion:reduce){.f i.flip{animation:none}.live::before{animation:none}}
</style></head><body>
<div class="wrap">
 <main class="board" id="board">
  <i class="screw a"></i><i class="screw b"></i><i class="screw c"></i><i class="screw d"></i>
  <div class="plate"><h1>Salas en línea · Patapon 3</h1></div>
  <section class="counters" aria-hidden="true">
   <div class="counter"><div class="word" data-c="players"></div><div class="cap">Jugadores</div></div>
   <div class="counter"><div class="word" data-c="rooms"></div><div class="cap">Salas</div></div>
   <div class="counter"><div class="word" data-c="vs"></div><div class="cap">VS en curso</div></div>
  </section>
  <div class="rows" id="rows" aria-hidden="true"></div>
  <p class="sr" id="live" role="status" aria-live="polite">Cargando las salas…</p>
  <ul class="sr" id="list"></ul>
 </main>
 <div class="foot"><span class="live" id="stamp">Cargando…</span>
  <a href="/">¿Quieres jugar? Descarga la beta</a></div>
</div>
<script>
(()=>{
const $=id=>document.getElementById(id);
const REDUCE=matchMedia('(prefers-reduced-motion:reduce)').matches;
const GLYPHS=Array.from('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789');
const STATE={open:['ABIERTA','open'],full:['LLENA','dim'],departing:['SALIENDO','search'],
 in_mission:['EN MISIÓN','active'],in_vs:['EN VS','active'],searching:['BUSCANDO','search'],playing:['JUGANDO','active']};
const NAME_CELLS=14,STATE_CELLS=9;

/* a word = fixed number of flap cells; a changed cell flips through a few glyphs and lands */
function makeWord(el,len){
 el.replaceChildren();el._c=[];
 for(let k=0;k<len;k++){const f=document.createElement('span');f.className='f';const i=document.createElement('i');
  f.append(i);el.append(f);el._c.push({i,ch:'',busy:0})}
}
function setCell(c,ch,delay){
 if(c.ch===ch)return;c.ch=ch;
 if(REDUCE){c.i.textContent=ch;return}
 const token=++c.busy,steps=ch===' '&&!c.i.textContent?0:3+(Math.random()*3|0);
 let n=0;
 const tick=()=>{
  if(c.busy!==token)return;
  const last=n>=steps;
  c.i.classList.remove('flip');void c.i.offsetWidth;c.i.classList.add('flip');
  setTimeout(()=>{if(c.busy===token)c.i.textContent=last?ch:GLYPHS[Math.random()*GLYPHS.length|0]},45);
  if(!last){n++;setTimeout(tick,95)}
 };
 setTimeout(tick,delay)
}
function setWord(el,text,len,delay=0){
 if(!el._c||el._c.length!==len)makeWord(el,len);
 const chars=Array.from(text);
 for(let k=0;k<len;k++)setCell(el._c[k],k<chars.length?chars[k]:' ',delay+k*28)
}
const fit=(s,len)=>{const a=Array.from(String(s||'?').toUpperCase());
 return a.length>len?a.slice(0,len-1).join('')+'…':a.join('')};
const num=(n,len)=>String(n).padStart(len,' ');

const rowsEl=$('rows'),rowEls=new Map();
function rowFor(id){
 let r=rowEls.get(id);
 if(r)return r;
 r=document.createElement('div');r.className='row';
 r.innerHTML='<span class="tag"></span><div class="word n"></div><div class="word p"></div><div class="word s"></div><div class="det"></div>';
 r._tag=r.children[0];r._n=r.children[1];r._p=r.children[2];r._s=r.children[3];r._d=r.children[4];
 rowEls.set(id,r);rowsEl.append(r);return r
}
let first=true,order=0;
function paintRoom(r,i){
 const row=rowFor(r.key),d=first?i*60:0;
 const [label,tone]=STATE[r.state]||[String(r.state).toUpperCase(),'dim'];
 row._tag.className='tag '+r.kind;row._tag.textContent=r.kind==='vs'?'VS':'Coop';
 setWord(row._n,fit(r.owner,NAME_CELLS),NAME_CELLS,d);
 setWord(row._p,r.members+'/'+r.max_slot,3,d+120);
 setWord(row._s,label,STATE_CELLS,d+200);
 row._s.className='word s t-'+tone;row._p.className='word p t-'+(r.members>=r.max_slot?'dim':'open');
 row._n.className='word n';
 if(r.kind==='vs'){
  const bits=['Arena <b>'+(+r.mission)+'</b>','nivel <b>'+(+r.level)+'</b>'];
  if(r.rule_type===0&&r.rule_value)bits.push('<b>'+Math.round(r.rule_value/60)+'</b> min');
  row._d.innerHTML=bits.join(' · ')
 }else row._d.textContent=r.state==='in_vs'?'Su grupo está en una sala VS':'Cooperativo';
 return row
}
function placeholder(left,right=''){
 rowsEl.querySelectorAll('.row').forEach(x=>x.remove());rowEls.clear();
 const row=rowFor('msg');row._tag.className='tag';row._tag.textContent='';
 setWord(row._n,left,NAME_CELLS);setWord(row._p,'',3);setWord(row._s,right,STATE_CELLS,150);
 row._n.className='word n t-dim';row._s.className='word s t-dim';row._d.textContent='';
}

const cw={players:$('board').querySelector('[data-c=players]'),rooms:$('board').querySelector('[data-c=rooms]'),
 vs:$('board').querySelector('[data-c=vs]')};
function counters(p,r,v){
 const f=n=>n===null?'':num(n,2),w=n=>n===null?2:Math.max(2,String(n).length);
 setWord(cw.players,f(p),w(p),first?60:0);
 setWord(cw.rooms,f(r),w(r),first?140:0);
 setWord(cw.vs,f(v),w(v),first?220:0)
}
const clock=t=>new Date(t*1000).toLocaleTimeString('es-MX',{hour:'2-digit',minute:'2-digit',second:'2-digit'});
let shown='';
async function load(){
 try{
  const j=await (await fetch('/salas.json',{cache:'no-store'})).json();
  if(!j.ok){
   if(shown!=='off'){counters(null,null,null);placeholder('SIN SEÑAL');shown='off'}
   $('live').textContent='El servidor online no está respondiendo ahora.';$('list').replaceChildren();
   $('stamp').className='live off';$('stamp').textContent='Sin señal · reintentando';first=false;return}
  const rooms=j.rooms,vs=rooms.filter(r=>r.kind==='vs'&&r.state==='playing').length;
  counters(j.players,rooms.length,vs);
  const seen={};rooms.forEach(r=>{const k=r.owner+'/'+r.kind;seen[k]=(seen[k]||0)+1;r.key=k+(seen[k]>1?'/'+seen[k]:'')});
  if(shown==='off'||shown==='empty'){rowsEl.querySelectorAll('.row').forEach(x=>x.remove());rowEls.clear()}
  if(!rooms.length){
   placeholder(j.players?'SIN SALAS':'NADIE POR AQUÍ');shown='empty'
  }else{
   shown='rooms';
   rooms.forEach((r,i)=>paintRoom(r,i));
   for(const [key,el] of [...rowEls])if(!rooms.some(r=>r.key===key)){el.remove();rowEls.delete(key)}
   rooms.forEach(r=>rowsEl.append(rowEls.get(r.key)))
  }
  const list=$('list');list.replaceChildren(...rooms.map(r=>{const li=document.createElement('li');
   li.textContent=(r.kind==='vs'?'VS de ':'Cuartel de ')+r.owner+', '+r.members+' de '+r.max_slot+', '+
    (STATE[r.state]?STATE[r.state][0].toLowerCase():r.state)+(r.kind==='vs'?', arena '+r.mission+', nivel '+r.level:'');return li}));
  $('live').textContent=j.players+(j.players===1?' jugador':' jugadores')+' en línea, '+rooms.length+
   (rooms.length===1?' sala':' salas')+(vs?', '+vs+' VS en curso':'')+'.';
  $('stamp').className='live';$('stamp').textContent='En vivo · '+clock(j.t);first=false
 }catch(_){$('stamp').className='live off';$('stamp').textContent='Sin conexión · reintentando'}
}
makeWord(cw.players,2);makeWord(cw.rooms,2);makeWord(cw.vs,2);
load();setInterval(load,5000);
})();
</script></body></html>
"""

REPORT_PAGE = """<!doctype html><html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Reportar un problema</title>
<style>%s</style></head><body><main>
<h1>Reportar un problema</h1>
<p>Beta del modo online de Patapon 3. Si el launcher abre, mejor usa su botón «Reportar problema»: manda también el
registro del juego. Este formulario es para cuando no abre o estás en el teléfono.</p>
<form id="f">
<label for="what">¿Qué pasó?</label>
<textarea id="what" maxlength="4000" required></textarea>
<label for="when">¿Cuándo, más o menos?</label>
<input id="when" type="text" maxlength="100" placeholder="sábado 22:15">
<label for="mode">¿Dónde?</label>
<select id="mode"><option value="escondite">Escondite (hideout)</option><option value="coop">Misión en co-op</option>
<option value="vs">VS</option><option value="otro" selected>Otro</option></select>
<label for="nick">Tu nombre en el juego (opcional)</label>
<input id="nick" type="text" maxlength="32">
<label for="code">Tu código de invitación (opcional)</label>
<input id="code" type="text" maxlength="64" autocomplete="off">
<button id="b" type="submit">Enviar reporte</button>
<div id="msg" role="status"></div>
</form></main>
<script>
const f=document.getElementById('f'),m=document.getElementById('msg'),b=document.getElementById('b');
const v=id=>document.getElementById(id).value;
f.addEventListener('submit',async e=>{e.preventDefault();m.className='';m.textContent='';
 if(!v('what').trim()){m.className='err';m.textContent='Cuéntanos qué pasó.';return}
 b.disabled=true;
 try{const r=await fetch('/report',{method:'POST',headers:{'Content-Type':'application/json'},
   body:JSON.stringify({source:'web',what:v('what'),when:v('when'),mode:v('mode'),nick:v('nick'),code:v('code')})});
  const j=await r.json().catch(()=>({}));
  if(r.ok){m.className='ok';m.textContent='Reporte enviado (#'+j.id+'). ¡Gracias!';f.reset()}
  else if(r.status===429){m.className='err';m.textContent='Demasiados reportes seguidos; intenta en una hora.'}
  else{m.className='err';m.textContent='No se pudo enviar ('+(j.error||r.status)+').'}}
 catch(_){m.className='err';m.textContent='Sin conexión; intenta de nuevo.'}
 b.disabled=false});
</script></body></html>
""" % PAGE_STYLE


# ---------------------------------------------------------------------------------------------------------- selftest

def selftest():
    import http.client
    import tempfile

    tmp = tempfile.mkdtemp(prefix="p3beta_")
    db = os.path.join(tmp, "beta.db")
    cfg = os.path.join(tmp, "beta_config.json")
    with open(cfg, "w", encoding="utf-8") as f:
        json.dump({"codes": [{"label": "invitado 07", "sha256": code_hash("P3-K7QF-2MXD"), "enabled": True,
                              "staff": False}]}, f)
    status = os.path.join(tmp, "salas.json")
    svc = Service("127.0.0.1", 0, db, cfg, status=status)
    threading.Thread(target=svc.serve_forever, daemon=True).start()
    port = svc.server_address[1]

    def req(method, path, body=None, host="report.x", addr="10.0.0.1", raw=None):
        c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
        headers = {"Host": host, "CF-Connecting-IP": addr}
        if data is not None:
            headers["Content-Type"] = "application/json"
        c.request(method, path, data, headers)
        r = c.getresponse()
        text = r.read().decode("utf-8", "replace")
        c.close()
        return r.status, text

    def rows(sql, *params):
        with sqlite3.connect(db) as c:
            return c.execute(sql, params).fetchall()

    try:
        # report_stored
        logtext = "línea 1\n" + "x" * 5000 + "\nINVITE_UNKNOWN fin\n"
        st, text = req("POST", "/report", {"source": "launcher", "what": "se congeló en VS", "when": "21:30",
                                           "mode": "vs", "nick": "Edson", "code": " p3-k7qf-2mxd ",
                                           "version": "0.2.0-beta.1", "machine": "abc123",
                                           "log": base64.b64encode(logtext.encode()).decode()})
        assert st == 200, (st, text)
        rid = json.loads(text)["id"]
        r = rows("SELECT source, label, nick, mode, when_text, what, version, machine, log_z FROM reports WHERE id=?",
                 rid)
        assert len(r) == 1, r
        assert r[0][:8] == ("launcher", "invitado 07", "Edson", "vs", "21:30", "se congeló en VS", "0.2.0-beta.1",
                            "abc123"), r[0][:8]
        assert zlib.decompress(r[0][8]).decode() == logtext
        assert "P3K7QF2MXD" not in str(rows("SELECT * FROM reports")), "the code itself is never stored"
        st, text = req("POST", "/report", {"source": "web", "what": "no abre", "when": "ayer", "mode": "otro"})
        assert st == 200, (st, text)
        assert rows("SELECT label, log_z FROM reports WHERE id=?", json.loads(text)["id"]) == [(None, None)]

        # report_limits
        st, _ = req("POST", "/report", raw=b"{" + b" " * (2 * 1024 * 1024) + b"}", addr="10.0.0.2")
        assert st == 413, st
        st, _ = req("POST", "/report", {"source": "web", "what": "x", "when": "", "mode": "arena"}, addr="10.0.0.2")
        assert st == 400, st
        st, _ = req("POST", "/report", {"source": "web", "what": "", "when": "", "mode": "otro"}, addr="10.0.0.2")
        assert st == 400, st
        st, _ = req("POST", "/report", {"source": "web", "what": "x" * 4001, "when": "", "mode": "otro"},
                    addr="10.0.0.2")
        assert st == 400, st
        big = base64.b64encode(b"y" * (MAX_LOG + 1)).decode()
        st, _ = req("POST", "/report", {"source": "launcher", "what": "x", "when": "", "mode": "otro", "log": big},
                    addr="10.0.0.2")
        assert st in (400, 413), st
        st, _ = req("POST", "/report", {"source": "web", "what": "x", "when": "", "mode": "otro", "log": "%%%"},
                    addr="10.0.0.2")
        assert st == 400, st
        for i in range(10):
            st, _ = req("POST", "/report", {"source": "web", "what": "n%d" % i, "when": "", "mode": "otro"},
                        addr="10.0.0.3")
            assert st == 200, (i, st)
        st, _ = req("POST", "/report", {"source": "web", "what": "n10", "when": "", "mode": "otro"}, addr="10.0.0.3")
        assert st == 429, st
        st, _ = req("POST", "/report", {"source": "web", "what": "other", "when": "", "mode": "otro"},
                    addr="10.0.0.4")
        assert st == 200, st
        svc.clock = lambda: time.time() + 3601
        st, _ = req("POST", "/report", {"source": "web", "what": "an hour later", "when": "", "mode": "otro"},
                    addr="10.0.0.3")
        assert st == 200, st
        svc.clock = time.time

        # form_served
        st, text = req("GET", "/", host="report.montick.app")
        assert st == 200 and "Reportar un problema" in text and "/report" in text, (st, text[:200])
        st, text = req("GET", "/", host="beta.montick.app")
        assert st == 200 and DOWNLOAD_URL in text and "abierta a todos" in text, (st, text[:200])
        assert "domingo 1 de noviembre a las 23:59" in text and "apodo" in text
        assert "Ejecutar de todas formas" in text and "Restaurar" in text and "Exclusiones" in text, "blocked-file help"
        assert "UCES01421" in text and "DxD" in text and "UCUS98751" in text, "which ISO works is said up front"
        st, _ = req("GET", "/nada", host="beta.montick.app")
        assert st == 404, st

        # queue_signup
        st, text = req("POST", "/cola", {"email": "luis@example.com", "consent": True, "website": ""},
                       host="beta.montick.app", addr="10.1.0.1")
        assert st == 200 and json.loads(text) == {"ok": True, "message": SIGNUP_OK}, (st, text)
        assert rows("SELECT email, invited_utc, label FROM queue") == [("luis@example.com", None, None)]

        # queue_dedup
        for e in ("Ana@Mail.com ", "ana@mail.com"):
            st, text = req("POST", "/cola", {"email": e, "consent": True, "website": ""}, host="beta.montick.app",
                           addr="10.1.0.2")
            assert st == 200 and json.loads(text)["ok"] is True, (e, st, text)
        assert rows("SELECT email FROM queue WHERE email LIKE 'ana%'") == [("ana@mail.com",)]

        # queue_rejects
        st, _ = req("POST", "/cola", {"email": "x@y.com", "consent": False, "website": ""}, host="beta.montick.app",
                    addr="10.1.0.3")
        assert st == 400, st
        for bad in ("no-arroba", "a@b", "a b@c.com", "x" * 250 + "@c.com", 7):
            st, _ = req("POST", "/cola", {"email": bad, "consent": True, "website": ""}, host="beta.montick.app",
                        addr="10.1.0.3")
            assert st == 400, (bad, st)
        for i in range(5):
            st, _ = req("POST", "/cola", {"email": "p%d@example.com" % i, "consent": True, "website": ""},
                        host="beta.montick.app", addr="10.1.0.4")
            assert st == 200, (i, st)
        st, _ = req("POST", "/cola", {"email": "p5@example.com", "consent": True, "website": ""},
                    host="beta.montick.app", addr="10.1.0.4")
        assert st == 429, st
        assert rows("SELECT COUNT(*) FROM queue WHERE email='p5@example.com'") == [(0,)]

        # queue_honeypot
        st, text = req("POST", "/cola", {"email": "bot@spam.com", "consent": True, "website": "http://spam"},
                       host="beta.montick.app", addr="10.1.0.5")
        assert st == 200 and json.loads(text) == {"ok": True, "message": SIGNUP_OK}, (st, text)
        assert rows("SELECT COUNT(*) FROM queue WHERE email='bot@spam.com'") == [(0,)]

        # lobby_page: beta.montick.app/salas polls /salas.json; the sign-up page links to it
        st, text = req("GET", "/salas", host="beta.montick.app")
        assert st == 200 and "/salas.json" in text and "jugadores" in text, (st, text[:200])
        st, text = req("GET", "/", host="beta.montick.app")
        assert 'href="/salas"' in text, "sign-up page links to the lobby"
        st, _ = req("GET", "/salas", host="report.montick.app")
        assert st == 404, st

        # lobby_json: the infrastructure server's --status file, fresh; missing or stale -> ok false
        st, text = req("GET", "/salas.json", host="beta.montick.app")
        assert st == 200 and json.loads(text) == {"ok": False}, (st, text)
        snap = {"t": round(time.time()), "players": 3, "rooms": [
            {"kind": "vs", "owner": "Montick", "members": 1, "max_slot": 2, "state": "searching", "mission": 3,
             "level": 40, "rule_type": 0, "rule_value": 900}]}
        with open(status, "w", encoding="utf-8") as f:
            json.dump(snap, f)
        st, text = req("GET", "/salas.json", host="beta.montick.app")
        assert st == 200 and json.loads(text) == dict(snap, ok=True), (st, text)
        with open(status, "w", encoding="utf-8") as f:
            json.dump(dict(snap, t=round(time.time()) - STATUS_STALE - 5), f)
        st, text = req("GET", "/salas.json", host="beta.montick.app")
        assert json.loads(text) == {"ok": False}, text
        with open(status, "w", encoding="utf-8") as f:
            f.write("{roto")
        st, text = req("GET", "/salas.json", host="beta.montick.app")
        assert st == 200 and json.loads(text) == {"ok": False}, (st, text)
    finally:
        svc.shutdown()
        svc.server_close()
    print("selftest OK")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=27322)
    ap.add_argument("--db", default="beta.db")
    ap.add_argument("--beta", help="the infrastructure server's beta config (invite code labels)")
    ap.add_argument("--status", default="salas.json",
                    help="the infrastructure server's --status file, served at beta.<host>/salas")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    svc = Service(args.host, args.port, args.db, args.beta, status=args.status)
    if disable_quick_edit():
        log("console QuickEdit turned off: a click in this window no longer pauses the service")
    log("beta reports + queue: http://%s:%d, db %s" % (args.host, svc.server_address[1], os.path.abspath(args.db)))
    try:
        svc.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
