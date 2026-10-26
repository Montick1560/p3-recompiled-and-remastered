#!/usr/bin/env python3
"""Sign-up queue and invite codes of the Patapon 3 online beta (stdlib only; run by the organiser on the server
machine, next to infra_server.py and beta_report_server.py).

    python beta_cola.py init beta_config.json [--staff 2]       new config: window, limits, staff codes (printed once)
    python beta_cola.py listar beta.db                          the queue (who signed up, who was drawn)
    python beta_cola.py sortear beta.db beta_config.json N --out tanda1.csv
                                                                draw N people not invited yet: one code each, added to
                                                                the config (the running server takes them live), CSV
                                                                correo,codigo,label to email by hand
    python beta_cola.py codigos beta_config.json 15 --out directos.csv [--prefijo directo]
                                                                N codes outside the queue ("directo 01"..), live
    python beta_cola.py desactivar beta_config.json "invitado 07"  disable a code (live)
    python beta_cola.py borrar-cola beta.db                     after the beta: delete every email of the queue
    python beta_cola.py --selftest

Codes look like P3-XXXX-XXXX (alphabet without 0/O/1/I). The config keeps only their SHA-256 and a label; the codes
themselves exist only in the CSV of the batch that drew them (and, for staff, in this command's output).
"""
import argparse
import csv
import hashlib
import json
import os
import re
import secrets
import sqlite3
import sys
import time

ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
STARTS, ENDS = "2026-10-24T03:00:00Z", "2026-11-02T05:59:59Z"  # Fri 23 Oct 21:00 .. Sun 1 Nov 23:59:59 CDMX
QUEUE_SCHEMA = ("CREATE TABLE IF NOT EXISTS queue(id INTEGER PRIMARY KEY, t_utc TEXT, email TEXT UNIQUE, "
                "invited_utc TEXT, label TEXT)")


# Same as tools/infra_server.py (that file runs with -I and is copied alone, so it is not imported); the selftest
# checks the three copies give the same hash.
def normalize_code(text):
    return re.sub(r"[\s-]", "", str(text)).upper()


def code_hash(text):
    return hashlib.sha256(normalize_code(text).encode()).hexdigest()


def new_code(taken):
    while True:
        raw = "".join(secrets.choice(ALPHABET) for _ in range(8))
        code = "P3-%s-%s" % (raw[:4], raw[4:])
        if code_hash(code) not in taken:
            taken.add(code_hash(code))
            return code


def utc_now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def read_config(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def write_config(path, cfg):
    """Temp file + rename: the server, which re-reads the file when it changes, never sees half of it."""
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=1)
    for attempt in range(20):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:  # Windows: the server is reading it this instant
            time.sleep(0.1)
    os.replace(tmp, path)


def init_config(path, staff=2):
    if os.path.exists(path):
        raise SystemExit("%s already exists: not overwritten" % path)
    taken = set()
    codes = [new_code(taken) for _ in range(staff)]
    cfg = {"starts": STARTS, "ends": ENDS, "min_proto": 2, "max_per_code": 2,
           "codes": [{"label": "staff %02d" % (i + 1), "sha256": code_hash(c), "enabled": True, "staff": True}
                     for i, c in enumerate(codes)]}
    write_config(path, cfg)
    return codes


def connect_queue(db):
    if not os.path.exists(db):
        raise SystemExit("%s does not exist (start beta_report_server.py with --db %s first)" % (db, db))
    c = sqlite3.connect(db, timeout=30)
    c.execute(QUEUE_SCHEMA)
    return c


def list_queue(db):
    with connect_queue(db) as c:
        return c.execute("SELECT id, t_utc, email, invited_utc, label FROM queue ORDER BY id").fetchall()


def draw(db, config, n, out):
    if os.path.exists(out):
        raise SystemExit("%s already exists: not overwritten (use another name, e.g. tanda2.csv)" % out)
    cfg = read_config(config)
    taken = {c["sha256"] for c in cfg["codes"]}
    numbers = [int(m.group(1)) for c in cfg["codes"] for m in [re.fullmatch(r"invitado (\d+)", c["label"])] if m]
    nxt = max(numbers, default=0) + 1
    c = connect_queue(db)
    try:
        free = c.execute("SELECT id, email FROM queue WHERE invited_utc IS NULL ORDER BY id").fetchall()
        picked = secrets.SystemRandom().sample(free, min(n, len(free)))
        rows = []
        for i, (rid, email) in enumerate(picked):
            rows.append((rid, email, new_code(taken), "invitado %02d" % (nxt + i)))
        cfg["codes"] += [{"label": label, "sha256": code_hash(code), "enabled": True, "staff": False}
                         for _, _, code, label in rows]
        write_config(config, cfg)  # the codes work from here on
        now = utc_now()
        with c:
            c.executemany("UPDATE queue SET invited_utc=?, label=? WHERE id=?",
                          [(now, label, rid) for rid, _, _, label in rows])
    finally:
        c.close()
    with open(out, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["correo", "codigo", "label"])
        w.writerows([(email, code, label) for _, email, code, label in rows])
    return [(email, code, label) for _, email, code, label in rows], len(free)


def direct_codes(config, n, out, prefix="directo"):
    """N invite codes outside the queue (people the organiser invites directly): labels "<prefix> NN" continue
    that prefix's numbering; CSV codigo,label."""
    if os.path.exists(out):
        raise SystemExit("%s already exists: not overwritten" % out)
    cfg = read_config(config)
    taken = {c["sha256"] for c in cfg["codes"]}
    pat = re.compile(re.escape(prefix) + r" (\d+)")
    nxt = max((int(m.group(1)) for c in cfg["codes"] for m in [pat.fullmatch(c["label"])] if m), default=0) + 1
    rows = [(new_code(taken), "%s %02d" % (prefix, nxt + i)) for i in range(n)]
    cfg["codes"] += [{"label": label, "sha256": code_hash(code), "enabled": True, "staff": False}
                     for code, label in rows]
    write_config(config, cfg)
    with open(out, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["codigo", "label"])
        w.writerows(rows)
    return rows


def disable(config, label):
    cfg = read_config(config)
    hit = [c for c in cfg["codes"] if c["label"] == label]
    if not hit:
        raise SystemExit("no code with label %r" % label)
    for c in hit:
        c["enabled"] = False
    write_config(config, cfg)


def clear_queue(db):
    with connect_queue(db) as c:
        n = c.execute("DELETE FROM queue").rowcount
    c = sqlite3.connect(db)
    c.execute("VACUUM")  # the deleted addresses do not stay in free pages of the file
    c.close()
    return n


# ---------------------------------------------------------------------------------------------------------- selftest

def selftest():
    import calendar
    import importlib.util
    import tempfile
    import threading

    here = os.path.dirname(os.path.abspath(__file__))

    def load(name):
        spec = importlib.util.spec_from_file_location(name, os.path.join(here, name + ".py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod
    infra = load("infra_server")
    report = load("beta_report_server")

    tmp = tempfile.mkdtemp(prefix="p3cola_")
    db, cfg_path = os.path.join(tmp, "beta.db"), os.path.join(tmp, "beta_config.json")
    c = sqlite3.connect(db)
    c.execute(QUEUE_SCHEMA)
    c.executemany("INSERT INTO queue(t_utc, email) VALUES (?, ?)", [("t", "p%d@example.com" % i) for i in range(5)])
    c.commit()
    c.close()

    staff = init_config(cfg_path)
    assert len(staff) == 2
    # init_no_overwrite
    try:
        init_config(cfg_path)
        raise AssertionError("init overwrote the config")
    except SystemExit:
        pass
    # draw_three
    got, free = draw(db, cfg_path, 3, os.path.join(tmp, "tanda1.csv"))
    assert free == 5 and len(got) == 3, (free, got)
    cfg = read_config(cfg_path)
    inv = [x for x in cfg["codes"] if not x["staff"]]
    assert [x["label"] for x in inv] == ["invitado 01", "invitado 02", "invitado 03"], inv
    assert all(x["enabled"] for x in inv)
    marked = [r for r in list_queue(db) if r[3]]
    assert len(marked) == 3 and sorted(r[4] for r in marked) == ["invitado 01", "invitado 02", "invitado 03"]
    with open(os.path.join(tmp, "tanda1.csv"), encoding="utf-8") as f:
        lines = list(csv.reader(f))
    assert lines[0] == ["correo", "codigo", "label"] and len(lines) == 4, lines
    assert {tuple(l) for l in lines[1:]} == {tuple(g) for g in got}
    try:
        draw(db, cfg_path, 1, os.path.join(tmp, "tanda1.csv"))
        raise AssertionError("draw overwrote a CSV")
    except SystemExit:
        pass
    # draw_skips_invited
    got2, free2 = draw(db, cfg_path, 5, os.path.join(tmp, "tanda2.csv"))
    assert free2 == 2 and len(got2) == 2, (free2, got2)
    assert not {g[0] for g in got} & {g[0] for g in got2}, "someone drawn twice"
    assert [g[2] for g in got2] == ["invitado 04", "invitado 05"], got2
    got3, _ = draw(db, cfg_path, 3, os.path.join(tmp, "tanda3.csv"))
    assert got3 == [], "nobody left"
    # codes_valid
    codes = staff + [g[1] for g in got + got2]
    assert all(re.fullmatch(r"P3-[%s]{4}-[%s]{4}" % (ALPHABET, ALPHABET), x) for x in codes), codes
    assert len(set(codes)) == len(codes)
    for x in codes + [" p3-k7qf-2mxd ", "P3K7QF2MXD"]:
        assert code_hash(x) == infra.code_hash(x) == report.code_hash(x), x
    assert {code_hash(x) for x in codes} == {x["sha256"] for x in read_config(cfg_path)["codes"]}
    assert not any(x in open(cfg_path, encoding="utf-8").read() for x in codes), "the config holds no code"

    # server_takes_new_codes: a running server logs in a code drawn after it started.
    lines_log = []
    t_in = calendar.timegm(time.strptime(STARTS, "%Y-%m-%dT%H:%M:%SZ")) + 3600
    srv = infra.Server("127.0.0.1", 0, None, logger=lines_log.append, beta_path=cfg_path, clock=lambda: t_in)
    th = threading.Thread(target=srv.serve, daemon=True)
    th.start()
    try:
        a = infra.TestClient(srv.port)
        a.ok("login", online_id="COLA1", proto=2, invite=got[0][1].lower())
        c = sqlite3.connect(db)
        c.execute("INSERT INTO queue(t_utc, email) VALUES ('t', 'late@example.com')")
        c.commit()
        c.close()
        time.sleep(1.1)  # a new mtime the server can tell apart
        late, _ = draw(db, cfg_path, 1, os.path.join(tmp, "tanda4.csv"))
        b = infra.TestClient(srv.port)
        deadline = time.monotonic() + 3
        while True:
            r = b.req("login", online_id="COLA2", proto=2, invite=late[0][1])
            if r["ok"]:
                break
            assert time.monotonic() < deadline, "the new code never worked: %r" % r
            time.sleep(0.2)
        # desactivar: live, the next login of that code is refused
        disable(cfg_path, late[0][2])
        d = infra.TestClient(srv.port)
        deadline = time.monotonic() + 3
        while True:
            r = d.req("login", online_id="COLA3", proto=2, invite=late[0][1])
            if not r["ok"] and r["error"] == infra.INVITE_DISABLED:
                break
            assert time.monotonic() < deadline, "disabled code still logs in: %r" % r
            if r["ok"]:
                d.close()
                d = infra.TestClient(srv.port)
            time.sleep(0.2)
        for cl in (a, b, d):
            cl.close()
    finally:
        srv.running = False
        th.join(timeout=3)
    try:
        disable(cfg_path, "invitado 99")
        raise AssertionError("disabled a missing label")
    except SystemExit:
        pass
    # direct_codes: invitations outside the queue, numbered per prefix, live in the config
    d1 = direct_codes(cfg_path, 3, os.path.join(tmp, "directos1.csv"), "directo")
    assert [x[1] for x in d1] == ["directo 01", "directo 02", "directo 03"], d1
    d2 = direct_codes(cfg_path, 2, os.path.join(tmp, "directos2.csv"), "directo")
    assert [x[1] for x in d2] == ["directo 04", "directo 05"], d2
    entries = {x["label"]: x for x in read_config(cfg_path)["codes"]}
    for code, label in d1 + d2:
        assert re.fullmatch(r"P3-[%s]{4}-[%s]{4}" % (ALPHABET, ALPHABET), code), code
        e = entries[label]
        assert e["sha256"] == code_hash(code) and e["enabled"] and not e["staff"], e
    with open(os.path.join(tmp, "directos1.csv"), encoding="utf-8") as f:
        assert list(csv.reader(f)) == [["codigo", "label"]] + [list(x) for x in d1]
    try:
        direct_codes(cfg_path, 1, os.path.join(tmp, "directos1.csv"), "directo")
        raise AssertionError("direct codes overwrote a CSV")
    except SystemExit:
        pass
    # borrar-cola
    assert clear_queue(db) == 6
    assert list_queue(db) == []
    with open(db, "rb") as f:
        assert b"p0@example.com" not in f.read(), "addresses left in the file"
    print("selftest OK")
    return 0


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("init")
    p.add_argument("config")
    p.add_argument("--staff", type=int, default=2)
    p = sub.add_parser("listar")
    p.add_argument("db")
    p = sub.add_parser("sortear")
    p.add_argument("db")
    p.add_argument("config")
    p.add_argument("n", type=int)
    p.add_argument("--out", required=True)
    p = sub.add_parser("codigos")
    p.add_argument("config")
    p.add_argument("n", type=int)
    p.add_argument("--out", required=True)
    p.add_argument("--prefijo", default="directo")
    p = sub.add_parser("desactivar")
    p.add_argument("config")
    p.add_argument("label")
    p = sub.add_parser("borrar-cola")
    p.add_argument("db")
    args = ap.parse_args()
    if args.cmd == "init":
        codes = init_config(args.config, args.staff)
        print("%s creado. Códigos de staff (funcionan también fuera de la ventana; guárdalos, no se vuelven a "
              "mostrar):" % args.config)
        for i, code in enumerate(codes):
            print("  staff %02d: %s" % (i + 1, code))
    elif args.cmd == "listar":
        rows = list_queue(args.db)
        print("%d en la cola, %d invitados, %d sin invitar" % (len(rows), sum(1 for r in rows if r[3]),
                                                                sum(1 for r in rows if not r[3])))
        for rid, t, email, invited, label in rows:
            print("%4d  %s  %-40s %s" % (rid, t, email, label or "-"))
    elif args.cmd == "sortear":
        got, free = draw(args.db, args.config, args.n, args.out)
        print("Sorteo: %d de %d personas sin invitar. Códigos añadidos a %s (el servidor los toma solo). "
              "Correos en %s." % (len(got), free, args.config, args.out))
    elif args.cmd == "codigos":
        rows = direct_codes(args.config, args.n, args.out, args.prefijo)
        print("%d códigos (%s .. %s) añadidos a %s (el servidor los toma solo). Códigos en %s."
              % (len(rows), rows[0][1], rows[-1][1], args.config, args.out) if rows else "0 códigos")
    elif args.cmd == "desactivar":
        disable(args.config, args.label)
        print("%s desactivado (el servidor lo aplica en un segundo; las sesiones abiertas siguen)." % args.label)
    elif args.cmd == "borrar-cola":
        n = clear_queue(args.db)
        print("Cola borrada: %d correos eliminados." % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
