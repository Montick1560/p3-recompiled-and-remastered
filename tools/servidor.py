#!/usr/bin/env python3
"""One window for the whole online server: game server, report and sign-up pages, and the Cloudflare tunnel.

    servidor.bat                 first run: starts everything
    servidor.bat                 while it runs: restarts on the new code once no game is left
    python -I servidor.py --selftest

A service that dies is started again; one that dies 3 times in a row within 30 s stops everything and says which.
The tunnel comes from cloudflared's config.yml. A cloudflared already running says nothing about it: this PC may run other tunnels as a service, and a second connector on the same tunnel is harmless.
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

DRAIN_FILE = "drain.flag"
LOCK_PORT = 27329
TUNNEL = ["cloudflared", "tunnel", "run"]


def service_commands():
    py = [sys.executable, "-I"]
    return {
        "infra": py + ["infra_server.py", "--host", "127.0.0.1", "--port", "27320", "--ws-port", "27321",
                       "--beta", "beta_config.json", "--db", "beta.db", "--log", "infra_server.log",
                       "--status", "salas.json", "--drain-file", DRAIN_FILE],
        "reportes": py + ["beta_report_server.py", "--host", "127.0.0.1", "--port", "27322", "--db", "beta.db",
                          "--beta", "beta_config.json"],
    }


def with_tunnel(commands, installed, log=print):
    if installed:
        return dict(commands, tunel=TUNNEL)
    log("AVISO: no encuentro cloudflared. Sin túnel nadie llega desde internet.")
    return commands


def acquire_lock(port):
    """Held for the life of the process; a second copy fails to bind and asks for a restart instead."""
    lock = socket.socket()
    try:
        lock.bind(("127.0.0.1", port))
    except OSError:
        lock.close()
        return None
    return lock


class Supervisor:
    def __init__(self, commands, drain_path=DRAIN_FILE, fast_fail_s=30, max_fast_fails=3, log=print):
        self.commands = commands
        self.drain_path = drain_path
        self.fast_fail_s = fast_fail_s
        self.max_fast_fails = max_fast_fails
        self.log = log
        self.procs = {}
        self.started_at = {}
        self.fast_fails = dict.fromkeys(commands, 0)
        self.starts = dict.fromkeys(commands, 0)

    def start(self, name):
        # shortcut: every service prints to this one window; each keeps its own log file if it had one.
        self.procs[name] = subprocess.Popen(self.commands[name])
        self.started_at[name] = time.monotonic()
        self.starts[name] += 1

    def start_all(self):
        for name in self.commands:
            self.start(name)

    def stop_all(self):
        for proc in self.procs.values():
            if proc.poll() is None:
                proc.terminate()
        for proc in self.procs.values():
            proc.wait(timeout=10)

    def restart_on_new_code(self):
        os.remove(self.drain_path)
        self.log("Sin partidas: reinicio con el código nuevo.")
        reportes = self.procs.get("reportes")
        if reportes:
            reportes.terminate()
            reportes.wait(timeout=10)
            self.start("reportes")
        self.start("infra")

    def step(self):
        """One look at the services. Returns an exit code once it has to stop, otherwise None."""
        for name, proc in list(self.procs.items()):
            if proc.poll() is None or self.procs[name] is not proc:  # alive, or replaced earlier in this same look
                continue
            if name == "infra" and os.path.exists(self.drain_path):
                self.restart_on_new_code()
                continue
            ran = time.monotonic() - self.started_at[name]
            self.fast_fails[name] = self.fast_fails[name] + 1 if ran < self.fast_fail_s else 0
            if self.fast_fails[name] >= self.max_fast_fails:
                self.log("ERROR: «%s» falló %d veces seguidas. Lee el error de arriba. Si dice que el puerto está en "
                         "uso, cierra la ventana vieja del servidor y vuelve a abrir esto." % (name, self.fast_fails[name]))
                self.stop_all()
                return 1
            self.log("«%s» se detuvo (código %s): lo arranco otra vez." % (name, proc.returncode))
            self.start(name)
        return None

    def run(self, poll_s=0.5):
        self.start_all()
        try:
            while True:
                code = self.step()
                if code is not None:
                    return code
                time.sleep(poll_s)
        except KeyboardInterrupt:
            self.log("Deteniendo todo...")
            self.stop_all()
            return 0


def launch(make_commands, drain_path, lock_port, log=print):
    lock = acquire_lock(lock_port)
    if lock is None:
        with open(drain_path, "w", encoding="utf-8"):
            pass
        log("Ya estaba corriendo: se reinicia con el código nuevo cuando no quede ninguna partida.")
        return 0
    try:
        return Supervisor(make_commands(), drain_path, log=log).run()
    finally:
        lock.close()


def selftest():
    py = [sys.executable, "-I", "-c"]
    forever, die = py + ["import time; time.sleep(120)"], py + ["raise SystemExit(3)"]
    logs = []
    tmp = tempfile.mkdtemp(prefix="p3servidor_")
    drain = os.path.join(tmp, DRAIN_FILE)

    def until(sup, cond, what):
        deadline = time.monotonic() + 10
        code = None
        while not cond() and time.monotonic() < deadline and code is None:
            code = sup.step()
            time.sleep(0.02)
        assert cond(), (what, logs)
        return code

    # selftest_starts_all
    sup = Supervisor({"infra": forever, "reportes": forever, "tunel": forever}, drain, log=logs.append)
    sup.start_all()
    assert all(p.poll() is None for p in sup.procs.values()), "all running"
    assert sup.step() is None

    # selftest_drain_restart: new code for the game server and the pages; the tunnel is not touched
    with open(drain, "w", encoding="utf-8"):
        pass
    tunnel, reportes = sup.procs["tunel"], sup.procs["reportes"]
    sup.procs["infra"].terminate()
    until(sup, lambda: sup.starts["infra"] == 2, "infra restarted")
    assert not os.path.exists(drain) and sup.starts["reportes"] == 2 and reportes.poll() is not None
    assert sup.starts["tunel"] == 1 and sup.procs["tunel"] is tunnel and tunnel.poll() is None

    # selftest_crash_restart: a service that ran a while is started again on its own
    sup.started_at["reportes"] -= 3600
    infra = sup.procs["infra"]
    sup.procs["reportes"].terminate()
    until(sup, lambda: sup.starts["reportes"] == 3, "reportes restarted")
    assert sup.procs["infra"] is infra and infra.poll() is None and sup.fast_fails["reportes"] == 0
    sup.stop_all()
    assert all(p.poll() is not None for p in sup.procs.values()), "stop_all ends every service"

    # selftest_fast_fail: 3 quick deaths stop everything and name the service; no endless restarts
    logs.clear()
    sup = Supervisor({"infra": forever, "reportes": die}, drain, log=logs.append)
    sup.start_all()
    code = None
    deadline = time.monotonic() + 20
    while code is None and time.monotonic() < deadline:
        code = sup.step()
        time.sleep(0.02)
    assert code == 1 and sup.starts["reportes"] == 3 and sup.procs["infra"].poll() is not None, (code, logs)
    assert "«reportes» falló 3" in logs[-1], logs

    # selftest_slow_failures_keep_going: only failures in a row count
    sup = Supervisor({"reportes": die}, drain, fast_fail_s=0, log=logs.append)
    sup.start_all()
    until(sup, lambda: sup.starts["reportes"] >= 5, "still restarting")
    sup.stop_all()

    # selftest_second_instance: asks for the restart, starts nothing and exits
    lock = acquire_lock(0)
    held = lock.getsockname()[1]
    assert acquire_lock(held) is None, "the port is taken"

    def boom():
        raise AssertionError("a second copy must not start services")

    logs.clear()
    assert launch(boom, drain, held, log=logs.append) == 0 and os.path.exists(drain)
    assert "cuando no quede ninguna partida" in logs[-1], logs
    os.remove(drain)
    lock.close()

    # selftest_tunnel_started and selftest_no_tunnel_binary
    base = {"infra": forever}
    assert with_tunnel(base, False, logs.append) == base and "no encuentro cloudflared" in logs[-1]
    assert with_tunnel(base, True, logs.append)["tunel"] == TUNNEL and base == {"infra": forever}

    # selftest_commands: the same services the old .bat files started
    cmds = service_commands()
    assert "--drain-file" in cmds["infra"] and "--status" in cmds["infra"] and "27322" in cmds["reportes"]

    shutil.rmtree(tmp, ignore_errors=True)
    print("selftest OK")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--selftest", action="store_true")
    if ap.parse_args().selftest:
        return selftest()
    return launch(lambda: with_tunnel(service_commands(), bool(shutil.which("cloudflared"))),
                  DRAIN_FILE, LOCK_PORT)


if __name__ == "__main__":
    sys.exit(main())
