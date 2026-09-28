"""Drive one user session through the lab LinBPQ's telnet port.

    python3 session.py LOGFILE [TO] [BBS] [TITLE]

Logs in to the lab LinBPQ (telnet 18023, test/test, callsign N0USR), connects
to BBS (default N0LMB, the linmail-pdn BBS, over AXUDP through pdn; LOCAL means
the lab LinBPQ's own BBS), sends a personal message to TO (default N0ABC),
lists it, reads it back and says bye. Everything seen is written to LOGFILE.
"""
import socket, time, sys, re

log = open(sys.argv[1], 'w')
DEST = sys.argv[2] if len(sys.argv) > 2 else 'N0ABC'
BBS = sys.argv[3] if len(sys.argv) > 3 else 'N0LMB'
s = socket.create_connection(('127.0.0.1', 18023))
s.settimeout(0.5)
buf = b''

def read_until(pat, timeout=60):
    global buf
    end = time.time() + timeout
    while time.time() < end:
        m = re.search(pat.encode(), buf, re.I)
        if m:
            out = buf[:m.end()]
            buf = buf[m.end():]
            log.write(out.decode('latin-1').replace('\r\n', '\n').replace('\r', '\n'))
            log.flush()
            return out
        try:
            d = s.recv(4096)
            if not d:
                break
            buf += d
        except socket.timeout:
            pass
    log.write(buf.decode('latin-1').replace('\r', '\n'))
    log.write('\n[TIMEOUT waiting for %r]\n' % pat)
    log.flush()
    raise SystemExit('timeout waiting for %r' % pat)

def send(line):
    log.write('[SEND] %s\n' % line)
    log.flush()
    s.sendall(line.encode() + b'\r\n')

read_until('user:')
send('test')
read_until('password:')
send('test')
read_until(r'\n')
time.sleep(1)
send('C 2 ' + BBS if BBS != 'LOCAL' else 'BBS')
read_until(r'>\s*$', 90)
send('SP ' + DEST)
read_until('Title', 30)
send(sys.argv[4] if len(sys.argv) > 4 else 'Proof of concept over pdn')
read_until('Message', 30)
send('Hello from a user connected to linmail-pdn through packet.net RHP.')
send('Second line of the message.')
send('/EX')
saved = read_until(r'>\s*$', 30)
msgno = re.search(rb'Message: (\d+)', saved).group(1).decode()
send('L')
read_until(r'>\s*$', 30)
send('R %s' % msgno)
read_until(r'>\s*$', 30)
send('B')
try:
    read_until('Disconnected|DISCONNECTED|\\}', 30)
except SystemExit:
    pass
time.sleep(2)
try:
    while True:
        d = s.recv(4096)
        if not d: break
        log.write(d.decode('latin-1').replace('\r', '\n'))
except Exception:
    pass
log.close()
