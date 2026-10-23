#!/usr/bin/env python3
"""Summary page of the Patapon 3 online beta, from its SQLite file (stdlib only).

Reads the metrics of tools/infra_server.py (--db: sessions, rooms, events), the problem reports and the sign-up
queue of tools/beta_report_server.py (reports, queue) and writes one Spanish HTML page for the organiser: sessions
per invite code, peak simultaneous sessions, rooms by kind, refused logins by reason, signaling and error counts,
queue size (never the addresses) and the reports, each report's game log written next to it.

    python -I tools/beta_summary.py beta.db resumen.html [--logs carpeta]
    python -I tools/beta_summary.py --selftest
"""
import argparse
import calendar
import collections
import html
import os
import sqlite3
import sys
import time
import zlib


def parse_utc(text):
    try:
        return calendar.timegm(time.strptime(text, "%Y-%m-%dT%H:%M:%SZ"))
    except (TypeError, ValueError):
        return None


def tables(c):
    return {r[0] for r in c.execute("SELECT name FROM sqlite_master WHERE type='table'")}


def esc(v):
    return html.escape("" if v is None else str(v))


def table(head, rows):
    out = ["<table><thead><tr>", "".join("<th>%s</th>" % esc(h) for h in head), "</tr></thead><tbody>"]
    for r in rows:
        out.append("<tr>" + "".join("<td>%s</td>" % c for c in r) + "</tr>")
    if not rows:
        out.append('<tr><td colspan="%d" class="muted">Nada todavía.</td></tr>' % len(head))
    out.append("</tbody></table>")
    return "".join(out)


def peak(intervals):
    """Most intervals open at once; an interval ending at t does not overlap one starting at t."""
    marks = sorted([(s, 1) for s, _ in intervals] + [(e, -1) for _, e in intervals], key=lambda m: (m[0], m[1]))
    cur = best = 0
    for _, d in marks:
        cur += d
        best = max(best, cur)
    return best


def hardware(log_text):
    """The runtime's "[RT] hardware: ..." line of a game log (the last one), without its prefix; None if absent."""
    found = None
    for line in log_text.splitlines():
        i = line.find("[RT] hardware: ")
        if i >= 0:
            found = line[i + len("[RT] hardware: "):].strip()
    return found


def summarize(db, logs_dir=None, now=None):
    now = time.time() if now is None else now
    c = sqlite3.connect(db)
    have = tables(c)
    parts = []
    stats = {}

    # sessions
    per_label = collections.OrderedDict()
    intervals = []
    if "sessions" in have:
        for label, staff, start, end, reason in c.execute(
                "SELECT label, staff, start_utc, end_utc, end_reason FROM sessions ORDER BY id"):
            s, e = parse_utc(start), parse_utc(end) if end else now
            if s is None or e is None:
                continue
            intervals.append((s, e))
            key = (label or "(sin código)") + (" (staff)" if staff else "")
            d = per_label.setdefault(key, {"n": 0, "min": 0.0, "reasons": collections.Counter()})
            d["n"] += 1
            d["min"] += max(0, e - s) / 60
            d["reasons"][reason or "abierta"] += 1
    stats["sessions"] = len(intervals)
    stats["peak"] = peak(intervals)
    stats["minutes"] = round(sum(d["min"] for d in per_label.values()))
    stats["players"] = sum(1 for k in per_label if not k.endswith("(staff)") and k != "(sin código)")

    rooms = collections.Counter()
    if "rooms" in have:
        for kind, n in c.execute("SELECT kind, COUNT(*) FROM rooms GROUP BY kind"):
            rooms[kind] = n
    refused = collections.Counter()
    ev = collections.Counter()
    errors = collections.Counter()
    if "events" in have:
        for kind, detail in c.execute("SELECT kind, detail FROM events"):
            ev[kind] += 1
            if kind == "refused":
                refused[(detail or "?").split(":")[0]] += 1
            elif kind == "error":
                errors[detail or "?"] += 1
    queue_total = queue_invited = 0
    if "queue" in have:
        queue_total, queue_invited = c.execute(
            "SELECT COUNT(*), COUNT(invited_utc) FROM queue").fetchone()
    reports = []
    if "reports" in have:
        reports = c.execute("SELECT id, t_utc, source, label, nick, mode, when_text, what, version, log_z "
                            "FROM reports ORDER BY id").fetchall()
    stats.update(rooms=dict(rooms), refused=dict(refused), signal_dead=ev["signal_dead"], errors=ev["error"],
                 queue=queue_total, invited=queue_invited, reports=len(reports))

    cards = [("Sesiones", stats["sessions"]), ("Jugadores con código", stats["players"]),
             ("Pico simultáneo", stats["peak"]), ("Minutos jugados", stats["minutes"]),
             ("Salas escondite", rooms.get("hideout", 0)), ("Salas VS", rooms.get("vs", 0)),
             ("Reportes", len(reports)), ("En la cola", queue_total), ("Invitados", queue_invited)]
    parts.append('<div class="cards">' + "".join(
        '<div class="card"><div class="n">%s</div><div class="l">%s</div></div>' % (esc(n), esc(l)) for l, n in cards)
        + "</div>")

    parts.append("<h2>Sesiones por código</h2>")
    parts.append(table(["Código", "Sesiones", "Minutos", "Cierres"], [
        (esc(k), d["n"], round(d["min"]), esc(", ".join("%s %d" % kv for kv in d["reasons"].most_common())))
        for k, d in per_label.items()]))

    parts.append("<h2>Problemas de conexión</h2>")
    parts.append(table(["Qué", "Veces"], [(esc(k), n) for k, n in [
        ("Señal perdida (signal_dead)", ev["signal_dead"]), ("Errores de peticiones", ev["error"])]]
        + [(esc("Login rechazado: " + k), n) for k, n in refused.most_common()]))
    if errors:
        parts.append("<h3>Errores más comunes</h3>")
        parts.append(table(["Petición y código", "Veces"], [(esc(k), n) for k, n in errors.most_common(10)]))

    parts.append("<h2>Reportes</h2>")
    rows = []
    cpus = collections.Counter()
    for rid, t_utc, source, label, nick, mode, when_text, what, version, log_z in reports:
        log_cell = "-"
        hw = hardware(zlib.decompress(log_z).decode("utf-8", "replace")) if log_z is not None else None
        if hw:
            cpus[hw[4:].split(" (")[0] if hw.startswith("CPU ") else "?"] += 1
        if log_z is not None and logs_dir:
            os.makedirs(logs_dir, exist_ok=True)
            name = "report_%d.log" % rid
            with open(os.path.join(logs_dir, name), "wb") as f:
                f.write(zlib.decompress(log_z))
            rel = os.path.relpath(os.path.join(logs_dir, name), os.path.dirname(os.path.abspath(OUT[0])) if OUT else ".")
            log_cell = '<a href="%s">%s</a>' % (esc(rel.replace(os.sep, "/")), esc(name))
        elif log_z is not None:
            log_cell = "sí"
        rows.append((rid, esc(t_utc), esc(label or "-"), esc(nick or "-"), esc(mode), esc(when_text),
                     '<div class="what">%s</div>' % esc(what), esc(source), esc(version or "-"),
                     '<div class="hw">%s</div>' % esc(hw or "-"), log_cell))
    parts.append(table(["#", "Hora (UTC)", "Código", "Nick", "Modo", "Cuándo", "Qué pasó", "Origen", "Versión",
                        "Equipo", "Log"], rows))
    stats["hardware"] = dict(cpus)
    c.close()

    page = PAGE % {"generated": esc(time.strftime("%Y-%m-%d %H:%M", time.localtime(now))),
                   "db": esc(os.path.basename(db)), "body": "\n".join(parts)}
    return page, stats


OUT = []  # the output page's path, for relative log links

PAGE = """<!doctype html><html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Resumen de la beta</title><style>
:root{--bg:#f6f1e7;--fg:#1d1a16;--muted:#6b6258;--card:#fffdf8;--line:#d9cfbf;--accent:#b3261e}
@media (prefers-color-scheme:dark){:root{--bg:#16130f;--fg:#efe8dc;--muted:#a59a8c;--card:#211d18;--line:#3a332b;
--accent:#ff6b5e}}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,sans-serif}
main{max-width:1100px;margin:0 auto;padding:24px 16px}h1{margin:0 0 4px}h2{margin:28px 0 8px}
.muted{color:var(--muted)}.cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px;
margin-top:16px}.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px}
.card .n{font-size:1.6rem;font-weight:700;font-variant-numeric:tabular-nums}.card .l{color:var(--muted)}
table{width:100%%;border-collapse:collapse;background:var(--card);border:1px solid var(--line)}
th,td{padding:6px 8px;border-bottom:1px solid var(--line);text-align:left;vertical-align:top}
th{font-weight:600}td{font-variant-numeric:tabular-nums}.what{white-space:pre-wrap;max-width:420px}
.hw{max-width:280px;font-size:.85em;color:var(--muted)}
a{color:var(--accent)}div.scroll{overflow-x:auto}
</style></head><body><main>
<h1>Resumen de la beta online</h1>
<p class="muted">Generado %(generated)s a partir de %(db)s. Sin correos ni direcciones IP.</p>
%(body)s
</main></body></html>
"""


# ---------------------------------------------------------------------------------------------------------- selftest

SCHEMA = """
CREATE TABLE sessions(id INTEGER PRIMARY KEY, label TEXT, staff INT, online_id TEXT, transport TEXT,
    proto INT, start_utc TEXT, end_utc TEXT, end_reason TEXT, bytes_in INT, bytes_out INT, dgrams_dropped INT);
CREATE TABLE rooms(id INTEGER PRIMARY KEY, kind TEXT, owner_session INT, created_utc TEXT, closed_utc TEXT,
    max_members INT);
CREATE TABLE events(t_utc TEXT, session INT, kind TEXT, room INT, detail TEXT);
CREATE TABLE reports(id INTEGER PRIMARY KEY, t_utc TEXT, source TEXT, label TEXT, nick TEXT, mode TEXT,
    when_text TEXT, what TEXT, version TEXT, machine TEXT, log_z BLOB);
CREATE TABLE queue(id INTEGER PRIMARY KEY, t_utc TEXT, email TEXT UNIQUE, invited_utc TEXT, label TEXT);
"""


def selftest():
    import tempfile
    tmp = tempfile.mkdtemp(prefix="p3sum_")
    db = os.path.join(tmp, "beta.db")
    c = sqlite3.connect(db)
    c.executescript(SCHEMA)
    s = [  # label, staff, start, end, reason
        ("invitado 01", 0, "2026-10-24T03:00:00Z", "2026-10-24T03:10:00Z", "closed"),
        ("invitado 01", 0, "2026-10-24T03:05:00Z", "2026-10-24T03:25:00Z", "idle"),
        ("invitado 02", 0, "2026-10-24T03:08:00Z", "2026-10-24T03:38:00Z", "closed"),
        ("staff 01", 1, "2026-10-24T03:30:00Z", "2026-10-24T03:31:00Z", "server_stop"),
    ]
    c.executemany("INSERT INTO sessions(label, staff, start_utc, end_utc, end_reason) VALUES (?,?,?,?,?)", s)
    c.executemany("INSERT INTO rooms(kind) VALUES (?)", [("hideout",)] * 3 + [("vs",)] * 2 + [("other",)])
    c.executemany("INSERT INTO events(kind, detail) VALUES (?, ?)",
                  [("refused", "invite unknown: ?")] * 4 + [("refused", "beta closed: invitado 03")]
                  + [("signal_dead", "x")] * 2 + [("error", "join 0x80550D13")] * 3 + [("join", None)] * 9)
    c.executemany("INSERT INTO queue(email, invited_utc) VALUES (?, ?)",
                  [("a@x.com", "t"), ("b@x.com", None), ("c@x.com", "t")])
    c.execute("INSERT INTO reports(t_utc, source, label, nick, mode, when_text, what, version, log_z) "
              "VALUES (?,?,?,?,?,?,?,?,?)", ("2026-10-24T04:00:00Z", "launcher", "invitado 01", "<b>Edson</b>", "vs",
                                             "21:30", "<script>alert(1)</script> se trabó", "0.2.0-beta.1",
                                             zlib.compress(b"linea del log\n[RT] hardware: CPU Intel(R) Core(TM) i3-6100 (4 threads), RAM 8192 MB, "
                                                           b"OS Windows 10 (build 19045), GPU Intel <HD> 530, GL 3.3, display "
                                                           b"1366x768 60 Hz\nINVITE_UNKNOWN\n")))
    c.execute("INSERT INTO reports(t_utc, source, mode, when_text, what) VALUES (?,?,?,?,?)",
              ("2026-10-24T05:00:00Z", "web", "otro", "", "no abre"))
    c.commit()
    c.close()
    OUT[:] = [os.path.join(tmp, "resumen.html")]
    page, st = summarize(db, os.path.join(tmp, "logs"), now=parse_utc("2026-10-24T06:00:00Z"))
    # summary_counts
    assert st["sessions"] == 4 and st["peak"] == 3 and st["players"] == 2, st
    assert st["minutes"] == 10 + 20 + 30 + 1, st
    assert st["rooms"] == {"hideout": 3, "vs": 2, "other": 1}, st
    assert st["refused"] == {"invite unknown": 4, "beta closed": 1}, st
    assert (st["signal_dead"], st["errors"], st["queue"], st["invited"], st["reports"]) == (2, 3, 3, 2, 2), st
    assert "<td>invitado 01</td><td>2</td><td>30</td>" in page, "per-code row"
    assert "a@x.com" not in page and "b@x.com" not in page, "no email on the page"
    with open(os.path.join(tmp, "logs", "report_1.log"), "rb") as f:
        assert f.read().endswith(b"60 Hz\nINVITE_UNKNOWN\n")
    assert 'href="logs/report_1.log"' in page, "log link"
    # report_hardware: the runtime's hardware line from the log, escaped, in the report's row
    assert "Intel(R) Core(TM) i3-6100 (4 threads), RAM 8192 MB" in page and "GPU Intel &lt;HD&gt; 530" in page, \
        "hardware of the report"
    assert st["hardware"] == {"Intel(R) Core(TM) i3-6100": 1}, st
    # summary_escapes_html
    assert "<script>alert(1)" not in page and "&lt;script&gt;alert(1)&lt;/script&gt;" in page
    assert "<b>Edson</b>" not in page and "&lt;b&gt;Edson&lt;/b&gt;" in page
    # a file with only the queue (the report service alone) still summarizes
    q = os.path.join(tmp, "solo_cola.db")
    c = sqlite3.connect(q)
    c.execute("CREATE TABLE queue(id INTEGER PRIMARY KEY, t_utc TEXT, email TEXT UNIQUE, invited_utc TEXT, label TEXT)")
    c.execute("INSERT INTO queue(email) VALUES ('z@x.com')")
    c.commit()
    c.close()
    _, st = summarize(q)
    assert st["queue"] == 1 and st["sessions"] == 0 and st["peak"] == 0, st
    print("selftest OK")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("db", nargs="?")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--logs", help="folder for each report's game log (report_<id>.log)")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.db or not args.out:
        ap.error("beta.db and out.html are required")
    OUT[:] = [args.out]
    page, st = summarize(args.db, args.logs)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(page)
    print("%s: %d sessions, peak %d, %d reports, queue %d (%d invited)"
          % (args.out, st["sessions"], st["peak"], st["reports"], st["queue"], st["invited"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
