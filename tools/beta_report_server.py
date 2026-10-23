#!/usr/bin/env python3
"""Problem reports and the sign-up queue of the Patapon 3 online beta (stdlib only).

One HTTP service behind the Cloudflare Tunnel, told apart by the Host header:
- `report.montick.app` (and any other host): `GET /` is the Spanish report form, `POST /report` takes a report
  from that form or from the launcher's "Reportar problema" button (with the tail of the game log);
- `beta.montick.app` (Host starting with `beta.`): `GET /` is the sign-up page (email + consent), `POST /cola`
  stores the email in the queue the user draws invitations from (tools/beta_cola.py). Nothing is sent to anyone.

Both write into the beta SQLite file the infrastructure server keeps its metrics in (tables `reports` and
`queue`). A report names its invite code by the code's label from the server's beta config, never the code.

    python -I tools/beta_report_server.py [--host 127.0.0.1] [--port 27322] [--db beta.db] [--beta beta_config.json]
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

    def __init__(self, host, port, db, beta=None, clock=time.time):
        self.db = db
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
        if self.path.split("?")[0] != "/":
            return self.answer(404, {"error": "not found"})
        page = SIGNUP_PAGE if self.queue_host() else REPORT_PAGE
        self.answer(200, page, "text/html; charset=utf-8")

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

SIGNUP_PAGE = """<!doctype html><html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Beta Patapon 3 online</title>
<style>%s</style></head><body><main>
<h1>Beta del modo online de Patapon 3</h1>
<p>Fin de semana de pruebas del modo online del port nativo para PC: del viernes 23 de octubre a las 21:00 al domingo
25 de octubre a las 23:59 (hora CDMX). Es para personas que tienen su propia copia (ISO) de Patapon 3.</p>
<p><strong>Qué ISO sirve:</strong> la versión europea (UCES01421) o la de Patapon 3 DxD. La versión americana
(UCUS98751) y la japonesa todavía no se pueden instalar; si solo tienes una de esas, mejor espera a la siguiente
ronda.</p>
<p>Apúntate con tu correo. Haremos un sorteo entre las personas apuntadas; si sales, te llegará un correo con tu
código de invitación y el enlace de descarga. No te mandaremos nada más.</p>
<form id="f" novalidate>
<label for="email">Correo</label>
<input id="email" name="email" type="email" maxlength="254" autocomplete="email" required>
<div class="hp" aria-hidden="true"><label for="website">No llenar</label>
<input id="website" name="website" type="text" tabindex="-1" autocomplete="off"></div>
<label class="check"><input id="consent" type="checkbox"> <span>%s.</span></label>
<button id="b" type="submit">Apuntarme</button>
<div id="msg" role="status"></div>
</form></main>
<script>
const f=document.getElementById('f'),m=document.getElementById('msg'),b=document.getElementById('b');
f.addEventListener('submit',async e=>{e.preventDefault();m.className='';m.textContent='';
 const email=document.getElementById('email').value.trim(),consent=document.getElementById('consent').checked;
 if(!email.includes('@')){m.className='err';m.textContent='Revisa tu correo.';return}
 if(!consent){m.className='err';m.textContent='Marca la casilla de consentimiento.';return}
 b.disabled=true;
 try{const r=await fetch('/cola',{method:'POST',headers:{'Content-Type':'application/json'},
   body:JSON.stringify({email,consent,website:document.getElementById('website').value})});
  const j=await r.json().catch(()=>({}));
  if(r.ok){m.className='ok';m.textContent=j.message;f.reset()}
  else{m.className='err';m.textContent=j.error||'No se pudo guardar; intenta de nuevo.'}}
 catch(_){m.className='err';m.textContent='Sin conexión; intenta de nuevo.'}
 b.disabled=false});
</script></body></html>
""" % (PAGE_STYLE, CONSENT)

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
    svc = Service("127.0.0.1", 0, db, cfg)
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
        assert st == 200 and CONSENT in text and 'name="website"' in text and "Apuntarme" in text, (st, text[:200])
        assert "/cola" in text
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
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    svc = Service(args.host, args.port, args.db, args.beta)
    if disable_quick_edit():
        log("console QuickEdit turned off: a click in this window no longer pauses the service")
    log("beta reports + queue: http://%s:%d, db %s" % (args.host, svc.server_address[1], os.path.abspath(args.db)))
    try:
        svc.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
