# SPDX-License-Identifier: GPL-3.0-or-later
"""Desk check of the multiapp host on the ESP32-S31-Function-CoreBoard-1
(doc/apps/multiapp-internals.md): the image and its FAT volume are on the board
(scripts/twister-flash.py), which boots into osmosdr_hs.

Run with the board's USB HS port J4 on the PC through a cable with VBUS cut
(apps/osmosdr_hs/README.rst):

    west twister -T apps/multiapp -s sample.esp_sdr.multiapp.s31.hil --device-testing \\
        --hardware-map tests/regression/hardware-map.yaml \\
        --flash-command "$PWD/scripts/twister-flash.py"
"""
import re
import subprocess

import pytest
from twister_harness import DeviceAdapter, Shell
from twister_harness.fixtures import determine_scope, get_ready_shell


@pytest.fixture(scope=determine_scope)
def shell(dut: DeviceAdapter) -> Shell:
    # Ctrl-C first: west dfu's CR LF leaves the shell swallowing the harness's bare LF
    # (tests/regression/conftest.py).
    dut.write(b"\x03")
    return get_ready_shell(dut)


def test_files(shell: Shell):
    out = shell.exec_command("multiapp list")
    apps = [line for line in out if re.match(r"\w+\s+(\d+ bytes|-)", line)]
    # hello, capture, rfprobe, osmosdr_hs and the radio's module.
    assert len(apps) == 5, out
    assert sum(", module" in line for line in apps) == 1, out
    for line in apps:
        # Every app has its file, the same as the built-in copy.
        assert re.search(r"file \d+ bytes", line) and "differs" not in line, line


def test_osmosdr_hs_from_file(shell: Shell):
    # The board's default app, with the vendor class for its stream.
    out = shell.exec_command("multiapp info")
    assert any(line.startswith("boot: osmosdr_hs, start 0, USB profile 0x6") for line in out), out
    assert any(line.startswith("osmosdr_hs (file):") for line in out), out


def test_radio_module(shell: Shell):
    # As a standalone image: IRAM_ATTR code in internal RAM, the rest of the code in PSRAM.
    out = shell.exec_command("multiapp info")
    assert any(re.match(r"module sdr \(file\): text \d+ at 0x5\S+, fast \d+ at 0x2f0", line)
               for line in out), out


def test_reset_was_clean(shell: Shell):
    out = shell.exec_command("multiapp crash")
    assert any("no fatal error" in line for line in out), out


# ---- The network (doc/dev/network-services.md): needs the board's Ethernet on a LAN with DHCP and the
# PC on the same LAN. The gate: `mcumgr arm` on the shell; files over MCUmgr (UDP port 4996).

import importlib.util
import time
import zlib
from pathlib import Path

LIB = Path(__file__).parents[3] / "scripts/west_commands/mcumgrlib.py"


def _client(lib, host):
    """An MCUmgr client with a first call that survives this PC's two interfaces in one subnet.

    The first IPv4 packets to the board after a quiet spell are lost until the neighbour entry has settled (the
    board and the PC are on the LAN through two interfaces of one subnet). A few pings first make it settle, and
    the client tries 6 times, 1.5 s apart: a board that does not answer still fails the test."""
    subprocess.run(["ping", "-c", "3", "-i", "0.3", "-W", "1", host], capture_output=True)
    return lib.Client(host, timeout=1.5, retries=6)


def _mcumgrlib():
    spec = importlib.util.spec_from_file_location("mcumgrlib", LIB)
    mod = importlib.util.module_from_spec(spec)
    import sys
    sys.modules["mcumgrlib"] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def lib():
    return _mcumgrlib()


@pytest.fixture(scope="module")
def board_ip(dut: DeviceAdapter) -> str:
    """The IPv4 address the board got from DHCP, read from its shell (up to 40 s after the boot)."""
    shell = get_ready_shell(dut)
    deadline = time.monotonic() + 40
    while time.monotonic() < deadline:
        for line in shell.exec_command("net ipv4"):
            # "DHCP <tab> preferred <tab> 1 <tab> a.b.c.d/255.255.255.0" (the address comes last)
            m = re.search(r"\bDHCP\b.*?\b(\d+\.\d+\.\d+\.\d+)/", line)
            if m and not m.group(1).startswith("0."):
                return m.group(1)
        time.sleep(2)
    pytest.fail("no IPv4 address from DHCP in 40 s: is the Ethernet cable on a LAN?")


def test_network_address(board_ip):
    # The board is on the LAN: DHCP gave it an address (the fixture waits for it).
    assert re.fullmatch(r"\d+\.\d+\.\d+\.\d+", board_ip)


def test_mcumgr_refuses_while_disarmed(shell: Shell, lib, board_ip):
    shell.exec_command("mcumgr disarm")
    cl = lib.Client(board_ip, timeout=1.0, retries=2)
    try:
        with pytest.raises(lib.McumgrError, match="access denied|no answer|gate"):
            cl.echo("hello")
    finally:
        cl.close()


def test_mcumgr_arm_echo(shell: Shell, lib, board_ip):
    out = shell.exec_command("mcumgr arm 10")
    assert any("armed" in line for line in out), out
    cl = _client(lib, board_ip)
    try:
        assert cl.echo("hello") == "hello"
    finally:
        cl.close()


@pytest.mark.parametrize("size", [6, 513, 20000])
def test_mcumgr_file_round_trip(shell: Shell, lib, board_ip, size):
    # A put, stat, crc and get of a file of the sizes below one chunk, over one chunk and many chunks; the loader
    # right after it (a file command gives the volume back at its end).
    shell.exec_command("mcumgr arm 10")
    data = bytes((i * 7 + size) & 0xFF for i in range(size))
    path = f"/FLASH:/hil{size}.bin"
    cl = _client(lib, board_ip)
    try:
        cl.fs_put(path, data)
        assert cl.fs_stat(path) == size
        assert cl.fs_crc(path) == zlib.crc32(data) & 0xFFFFFFFF
        assert cl.fs_get(path) == data
    finally:
        cl.close()
    out = shell.exec_command("multiapp list")
    assert any(re.match(r"\w+\s+(\d+ bytes|-)", line) for line in out), out


def test_mcumgr_path_outside_the_volume(shell: Shell, lib, board_ip):
    shell.exec_command("mcumgr arm 10")
    cl = _client(lib, board_ip)
    try:
        with pytest.raises(lib.McumgrError, match="access denied|/FLASH:/"):
            cl.fs_stat("/etc/passwd")
        with pytest.raises(lib.McumgrError, match="access denied|/FLASH:/"):
            cl.fs_stat("/FLASH:/../x")
    finally:
        cl.close()


# ---- SSH (doc/dev/network-services.md, phase 3): public key login only (ssh-ed25519 and
# ecdsa-sha2-nistp256 keys, no RSA), the Zephyr shell on the session, commands as `ssh <host> '<command>'`. Needs the PC's
# OpenSSH client. The board makes its ECDSA P-256 host key on the first boot (a moment) and keeps it. The test keys
# are made in a temp dir, added with `ssh key add` on the board's shell and removed at the end (six key slots).

import os
import shutil
import subprocess
import tempfile

SSH = shutil.which("ssh")
KEYGEN = shutil.which("ssh-keygen")
needs_ssh = pytest.mark.skipif(SSH is None or KEYGEN is None, reason="no OpenSSH client (nix-shell -p openssh)")
SSH_OPTS = ["-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR",
            "-o", "ConnectTimeout=40", "-o", "IdentitiesOnly=yes"]
TEST_COMMENT = "hil@esp-sdr-test"
KEY_TYPES = {"ed25519": [], "ecdsa": ["-b", "256"]}   # the types the board takes
KEYGEN_ARGS = {**KEY_TYPES, "rsa": ["-b", "3072"]}   # and one it does not


def _long_command(dut, shell, command, until, timeout=30):
    """Type a shell line that is wider than the terminal (80 columns) and wait for the line `until` (a regex).

    Shell.exec_command() waits for the echo of the command on one line, but the shell wraps the echo of a long
    line with cursor movements: so the line is written as it is, and what is waited for is the answer, then
    the prompt."""
    dut.write(command.encode() + b"\n")
    lines = dut.readlines_until(regex=until, timeout=timeout)
    dut.readlines_until(regex=re.escape(shell.prompt), timeout=timeout)
    return lines


def _reconnect(dut, timeout=90):
    """Take the DUT's serial connection to the board's CDC-ACM port again after the board reset.

    The port went away and came back: the harness's old handle is stale and closing it raises (an I/O error that
    would fail the test's teardown). Drop it without closing and open the port anew."""
    for conn in dut.connections:
        ser = getattr(conn, "_serial_connection", None)
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass
            conn._serial_connection = None
    deadline = time.monotonic() + timeout
    while True:
        try:
            dut.connect()
            return
        except Exception:
            if time.monotonic() > deadline:
                return    # the teardown has nothing to close then
            time.sleep(2)


def _keygen(path, key_type, comment):
    subprocess.run([KEYGEN, "-q", "-t", key_type, *KEYGEN_ARGS[key_type], "-N", "", "-C", comment, "-f", str(path)],
                   check=True)


def _ssh(host, key, remote=(), lines=(), wait=0.0, extra=(), env=None, timeout=90):
    """Run ssh with the private key `key`: `remote` is the command, `lines` are typed into it; (rc, stdout, stderr)."""
    cmd = [SSH, *SSH_OPTS, "-o", "BatchMode=yes", *extra]
    if key is not None:
        cmd += ["-i", str(key)]
    cmd += [host, *remote]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    try:
        for line in lines:
            p.stdin.write(line.encode() + b"\n")
            p.stdin.flush()
            time.sleep(1.0)
        time.sleep(wait)
        out, err = p.communicate(timeout=timeout)  # closes its input
    except subprocess.TimeoutExpired:
        p.kill()
        out, err = p.communicate()
    return p.returncode, out.decode(errors="replace"), err.decode(errors="replace")


def _client_fingerprint(host, known_hosts):
    """The host key fingerprint OpenSSH records on its first contact (the login itself may fail)."""
    subprocess.run([SSH, "-o", "StrictHostKeyChecking=accept-new", "-o", f"UserKnownHostsFile={known_hosts}",
                    "-o", "BatchMode=yes", "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=40",
                    "-o", "PreferredAuthentications=publickey", host, "true"],
                   capture_output=True, text=True, timeout=90)
    shown = subprocess.run([KEYGEN, "-lf", str(known_hosts)], capture_output=True, text=True, check=True).stdout
    return shown.split()[1], shown.split()[-1]


@pytest.fixture(scope="module")
def host_fingerprint(dut: DeviceAdapter, board_ip) -> str:
    """The board's host key fingerprint from its shell (`ssh hostkey`), once the key exists."""
    shell = get_ready_shell(dut)
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        for line in shell.exec_command("ssh hostkey"):
            m = re.match(r"ecdsa-sha2-nistp256 256 (SHA256:\S+)", line)
            if m:
                return m.group(1)
        time.sleep(3)
    pytest.fail("no SSH host key in 2 minutes (is ssh.conf in the build?)")


@pytest.fixture(scope="module")
def test_keys(dut: DeviceAdapter, shell: Shell, host_fingerprint):
    """One key pair of each type made here, their public lines added on the board's shell; removed at the end."""
    if SSH is None or KEYGEN is None:
        pytest.skip("no OpenSSH client")
    d = tempfile.mkdtemp(prefix="esdr-ssh-")
    keys = {}
    # leftovers of a run that died: the lines with this comment
    shell.exec_command(f"ssh key remove {TEST_COMMENT}", timeout=30)
    for key_type in KEY_TYPES:
        key = os.path.join(d, f"id_{key_type}")
        _keygen(key, key_type, TEST_COMMENT)
        line = open(key + ".pub").read().strip()
        out = _long_command(dut, shell, f'ssh key add "{line}"', r"key added|[Ff]ailed|refused|not ")
        assert any("key added" in l for l in out), out
        keys[key_type] = key
    yield keys
    shell.exec_command(f"ssh key remove {TEST_COMMENT}", timeout=30)
    shutil.rmtree(d, ignore_errors=True)


@needs_ssh
def test_ssh_host_key_is_the_one_clients_see(host_fingerprint, board_ip, tmp_path):
    # The fingerprint the board prints is the one OpenSSH records on its first contact, an ECDSA key.
    fp, kind = _client_fingerprint(board_ip, tmp_path / "known_hosts")
    assert fp == host_fingerprint and kind == "(ECDSA)"


@needs_ssh
def test_ssh_key_list_has_the_test_keys(shell: Shell, test_keys):
    out = shell.exec_command("ssh key list")
    for key_type, bits in (("ssh-ed25519", 256), ("ecdsa-sha2-nistp256", 256)):
        assert any(TEST_COMMENT in l and f"{key_type} {bits} SHA256:" in l and "NOT USED" not in l for l in out), out


@needs_ssh
@pytest.mark.parametrize("key_type", list(KEY_TYPES))
def test_ssh_login_with_every_key_type(test_keys, board_ip, key_type):
    # An exec request: the command runs in the Zephyr shell, its output comes back, the exit status is its.
    rc, out, err = _ssh(board_ip, test_keys[key_type], remote=["kernel version"])
    assert rc == 0 and "Zephyr version" in out, (rc, out, err)


@needs_ssh
def test_ssh_exec_returns_the_output_and_the_status(test_keys, board_ip):
    rc, out, err = _ssh(board_ip, test_keys["ed25519"], remote=["multiapp list"])
    assert rc == 0 and re.search(r"osmosdr_hs\s+\d+ bytes", out), (rc, out, err)
    # a prompt or an echo of the command is not output of the command
    assert "esdr:~$" not in out and "multiapp list" not in out, out
    rc, out, err = _ssh(board_ip, test_keys["ed25519"], remote=["no_such_command"])
    assert rc != 0 and rc != 255, (rc, out, err)


@needs_ssh
def test_ssh_mcumgr_arm_by_exec(test_keys, board_ip, lib):
    # The authenticated channel arms MCUmgr: the gate opens, an echo is answered, then it is closed again.
    rc, out, err = _ssh(board_ip, test_keys["ed25519"], remote=["mcumgr arm 1"])
    assert rc == 0 and "MCUmgr armed" in out, (rc, out, err)
    cl = _client(lib, board_ip)
    try:
        assert cl.echo("via ssh") == "via ssh"
    finally:
        cl.close()
    rc, out, err = _ssh(board_ip, test_keys["ed25519"], remote=["mcumgr disarm"])
    assert rc == 0 and "MCUmgr disarmed" in out, (rc, out, err)


@needs_ssh
def test_ssh_interactive_shell(test_keys, board_ip):
    # `ssh <host> shell` (an exec of the word shell) and a plain `ssh <host>` (a shell request) both reach the
    # interactive Zephyr shell, where the commands are typed; -T: there is no terminal here.
    for remote, extra in ((["shell"], []), ([], ["-T"])):
        rc, out, err = _ssh(board_ip, test_keys["ed25519"], remote=remote, extra=extra, lines=["kernel version"], wait=2)
        assert "Zephyr version" in out and "esdr:~$" in out, (remote, rc, out, err)


@needs_ssh
def test_ssh_unknown_key_is_refused(board_ip, host_fingerprint, test_keys, tmp_path):
    for key_type in KEYGEN_ARGS:
        other = tmp_path / f"other_{key_type}"
        _keygen(other, key_type, "not-added")
        rc, out, err = _ssh(board_ip, other, remote=["kernel version"])
        assert rc == 255 and "Permission denied" in err, (key_type, rc, out, err)
        assert "Zephyr version" not in out


@needs_ssh
def test_ssh_rsa_keys_are_not_taken(dut: DeviceAdapter, shell: Shell, tmp_path):
    # This build has no RSA: the line is understood and refused, nothing is written to the key file
    key = tmp_path / "rsa"
    _keygen(key, "rsa", TEST_COMMENT)
    line = open(str(key) + ".pub").read().strip()
    out = _long_command(dut, shell, f'ssh key add "{line}"', r"key added|RSA keys are not supported|refused")
    assert any("RSA keys are not supported" in l for l in out), out
    assert not any("key added" in l for l in out), out


@needs_ssh
def test_ssh_overlong_command_is_answered(test_keys, board_ip):
    # 255 characters at most: the answer is a line on stderr and a status, not a dropped connection
    rc, out, err = _ssh(board_ip, test_keys["ed25519"], remote=["echo " + "x" * 300])
    assert rc == 1 and "longer than 255" in err and out == "", (rc, out, err)


@needs_ssh
def test_ssh_password_is_not_offered(board_ip, host_fingerprint, tmp_path):
    # The server was started without a password: it lists publickey only as a method, so a client has no password to try.
    rc, out, err = _ssh(board_ip, None, remote=["kernel version"],
                        extra=["-o", "PubkeyAuthentication=no", "-o", "PreferredAuthentications=password"])
    assert rc == 255 and "Permission denied (publickey)" in err and "password" not in err, (rc, out, err)
    assert "Zephyr version" not in out


@needs_ssh
def test_ssh_subsystem_is_refused(test_keys, board_ip):
    # sftp is a later phase: the request fails and the session ends, with no shell behind it
    rc, out, err = _ssh(board_ip, test_keys["ed25519"], extra=["-s"], remote=["sftp"])
    assert rc != 0 and "Zephyr version" not in out, (rc, out, err)


@needs_ssh
def test_ssh_status_and_key_commands(shell: Shell, test_keys):
    out = shell.exec_command("ssh status")
    assert any("public key login only" in l for l in out), out
    assert any(re.search(r"keys: [2-6] loaded of 6 slots", l) for l in out), out
    out = shell.exec_command("ssh key add command=ls ssh-rsa AAAA x", timeout=20)
    assert any("options" in l for l in out), out


@needs_ssh
def test_ssh_fast_exec_cycles_all_succeed(shell: Shell, test_keys, board_ip, tmp_path):
    # Sessions one after the other as fast as they come: a key removed and added, a login with the key just added, a
    # command. Every one must work (a client that comes while the last session's connection is still closing waits
    # in the listen backlog) and the board must not crash: the session shell used to be made again while its thread
    # still lived (a fatal error in z_swap).
    key = tmp_path / "cycle"
    _keygen(key, "ecdsa", "esdr-test-cycle")
    line = open(str(key) + ".pub").read().strip()
    _ssh(board_ip, test_keys["ed25519"], remote=["ssh key remove esdr-test-cycle"])   # leftovers: may find none
    steps = (
        (test_keys["ed25519"], f"ssh key add {line}", "key added"),
        (key, "kernel version", "Zephyr version"),
        (test_keys["ed25519"], "multiapp list", "osmosdr_hs"),
        (test_keys["ed25519"], "ssh key remove esdr-test-cycle", "1 key removed"),
    )
    for i in range(20):
        for used, command, expected in steps:
            rc, out, err = _ssh(board_ip, used, remote=[command])
            assert rc == 0 and expected in out, (i, command[:30], rc, out, err)
    out = shell.exec_command("multiapp crash")
    assert any("no fatal error" in l for l in out), out


@needs_ssh
def test_ssh_concurrent_clients_queue(test_keys, board_ip):
    # One connection at a time: the others wait in the listen backlog (2) and are served in turn
    from concurrent.futures import ThreadPoolExecutor

    with ThreadPoolExecutor(3) as pool:
        jobs = [pool.submit(_ssh, board_ip, test_keys["ed25519"], remote=["kernel uptime"], timeout=120)
                for _ in range(3)]
        results = [j.result() for j in jobs]
    for rc, out, err in results:
        assert rc == 0 and "Uptime:" in out, (rc, out, err)


@needs_ssh
def test_ssh_killed_clients_do_not_wedge_the_shell(test_keys, board_ip):
    # A client that goes away in the middle of a command's output used to leave the shell's writer waiting for room in
    # a channel that was gone: every later exec was then answered "the previous command is still ending".
    import signal

    for delay in (0.3, 0.45, 0.6, 0.8, 1.0):
        p = subprocess.Popen([SSH, *SSH_OPTS, "-o", "BatchMode=yes", "-i", test_keys["ed25519"], board_ip,
                              "kernel thread list"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(delay)
        p.send_signal(signal.SIGKILL)
        p.communicate()
        time.sleep(1)
    rc, out, err = _ssh(board_ip, test_keys["ed25519"], remote=["kernel version"])
    assert rc == 0 and "Zephyr version" in out, (rc, out, err)


@needs_ssh
def test_ssh_idle_connection_does_not_block_a_login(test_keys, board_ip):
    # One slot: a TCP connection that says nothing holds it until the login grace time (30 s) is over; a real login
    # that comes meanwhile waits in the backlog and works after that, within the time and not much later.
    import socket

    idle = socket.create_connection((board_ip, 22), timeout=10)
    try:
        t0 = time.monotonic()
        rc, out, err = _ssh(board_ip, test_keys["ed25519"], remote=["kernel version"], timeout=120)
        waited = time.monotonic() - t0
    finally:
        idle.close()
    assert rc == 0 and "Zephyr version" in out, (rc, out, err)
    assert waited < 30 + 15, f"the login took {waited:.0f} s"


def _esdr_usb():
    for parent in Path(__file__).resolve().parents:
        tool = parent / "build-clanker" / "host" / "esdr_usb"
        if tool.exists():
            return tool
    return None


@needs_ssh
@pytest.mark.skipif(_esdr_usb() is None, reason="no build-clanker/host/esdr_usb")
def test_ssh_key_file_changes_are_refused_while_a_stream_runs(test_keys, board_ip):
    # Reading or writing the key file stops the flash cache: refused while osmosdr_hs streams (ESDR_USB_SERIAL
    # names the board when several are on the PC). Logins and the other commands go on.
    cmd = [str(_esdr_usb())]
    if os.environ.get("ESDR_USB_SERIAL"):
        cmd += ["-s", os.environ["ESDR_USB_SERIAL"]]
    cmd += ["rx", "-t", "25", "-r", "20000000", "-f", "2412000000"]
    stream = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(8)
        assert stream.poll() is None, "the stream did not start"
        for command in ("ssh keys reload", "ssh key remove esdr-test-nothing"):
            rc, out, err = _ssh(board_ip, test_keys["ed25519"], remote=[command])
            assert rc != 0 and "refused" in out + err, (command, rc, out, err)
        rc, out, err = _ssh(board_ip, test_keys["ed25519"], remote=["ssh key list"])
        assert rc == 0 and "hil@esp-sdr-test" in out, ("key list during the stream", rc, out, err)    # RAM only
    finally:
        # a program that is killed does not stop its stream cleanly: the board must notice that nobody reads
        stream.terminate()
        stream.wait(timeout=30)
    deadline = time.monotonic() + 10
    while True:
        rc, out, err = _ssh(board_ip, test_keys["ed25519"], remote=["ssh keys reload"])
        if rc == 0 or time.monotonic() > deadline:
            break
        time.sleep(0.5)
    assert rc == 0, ("10 s after the stream's program was killed", rc, out, err)


@needs_ssh
def test_ssh_host_key_survives_a_reboot(dut: DeviceAdapter, host_fingerprint, test_keys, board_ip, tmp_path):
    # Last: the board is reset through its SSH session and must come back with the same host key
    rc, out, err = _ssh(board_ip, test_keys["ed25519"], remote=["kernel reboot cold"], timeout=30)
    deadline = time.monotonic() + 120
    time.sleep(10)
    while time.monotonic() < deadline:
        try:
            fp, kind = _client_fingerprint(board_ip, tmp_path / "known_hosts_after")
            break
        except (subprocess.CalledProcessError, IndexError):
            (tmp_path / "known_hosts_after").unlink(missing_ok=True)
            time.sleep(5)
    else:
        _reconnect(dut)
        pytest.fail("the board did not answer on SSH 2 minutes after the reset")
    _reconnect(dut)
    assert fp == host_fingerprint
