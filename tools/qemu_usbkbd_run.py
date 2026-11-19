#!/usr/bin/env python3
"""Boot xv6 (raspi3b) in QEMU with a USB keyboard, log in on the serial
console, type a command through the *USB keyboard* (HMP sendkey) and save the
whole serial log.

  python3 tools/qemu_usbkbd_run.py [--topo hub|root] [--log out.log]
                                   [--text 'echo hi'] [--timeout 90]

--topo hub  (default) -device usb-hub + usb-kbd on hub port 1, like the real
            Pi 3 where the keyboard sits behind the LAN9514 hub.
--topo root usb-kbd directly on the DWC2 root port.

Run `make USBKBD_TRACE=2 ...` first for the full trace.
"""
import argparse, os, select, socket, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

KEYMAP = {' ': 'spc', '\n': 'ret', '-': 'minus', '/': 'slash', '.': 'dot',
          '=': 'equal', ',': 'comma', ';': 'semicolon', "'": 'apostrophe'}


def qemu_cmd(topo, mon):
    out = subprocess.check_output(['make', '-s', '-n', 'qemu'], cwd=ROOT,
                                  text=True).strip().splitlines()[-1]
    cmd = out.split()
    if topo == 'hub':
        cmd += ['-device', 'usb-hub,bus=usb-bus.0,port=1',
                '-device', 'usb-kbd,bus=usb-bus.0,port=1.1']
    else:
        cmd += ['-device', 'usb-kbd,bus=usb-bus.0']
    cmd += ['-monitor', 'unix:%s,server,nowait' % mon]
    return cmd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--topo', default='hub', choices=['hub', 'root'])
    ap.add_argument('--log', default='qemu-usbkbd.log')
    ap.add_argument('--text', default='echo usbkbd-ok\n')
    ap.add_argument('--timeout', type=float, default=120)
    a = ap.parse_args()
    a.text = a.text.encode().decode('unicode_escape')

    mon = os.path.join(tempfile.gettempdir(), 'xv6kbd%d.sock' % os.getpid())
    cmd = qemu_cmd(a.topo, mon)
    # drop the Makefile's "-monitor none" so our unix monitor is the only one
    i = cmd.index('-monitor')
    if cmd[i + 1] == 'none':
        del cmd[i:i + 2]
    print(' '.join(cmd), file=sys.stderr)
    p = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    buf = b''
    t0 = time.time()

    def pump(until=None, secs=1.0):
        nonlocal buf
        end = time.time() + secs
        while time.time() < end:
            r, _, _ = select.select([p.stdout], [], [], 0.1)
            if r:
                d = os.read(p.stdout.fileno(), 4096)
                if not d:
                    return False
                buf += d
                sys.stdout.buffer.write(d); sys.stdout.flush()
                if until and until.encode() in buf[-4096:]:
                    return True
        return False

    def serial(s):
        for ch in s:
            p.stdin.write(ch.encode()); p.stdin.flush(); time.sleep(0.03)

    def hmp(line):
        s = socket.socket(socket.AF_UNIX)
        s.connect(mon); s.sendall((line + '\n').encode()); time.sleep(0.05)
        s.close()

    try:
        pump('login:', a.timeout) or sys.exit('no login prompt')
        serial('root\n'); pump('assword', 10)
        serial('xv6\n'); pump('$ ', 15)
        time.sleep(1)
        buf += b'\n=== typing via USB keyboard (HMP sendkey) ===\n'
        for ch in a.text:
            k = KEYMAP.get(ch, ch)
            if ch.isupper():
                k = 'shift-' + ch.lower()
            hmp('sendkey %s 60' % k)
            pump(None, 0.15)
        pump('$ ', 8)
        pump(None, 2)
    finally:
        p.kill()
        with open(a.log, 'wb') as f:
            f.write(buf)
        try: os.unlink(mon)
        except OSError: pass
    print('\nlog -> %s (%.1fs)' % (a.log, time.time() - t0), file=sys.stderr)


if __name__ == '__main__':
    main()
