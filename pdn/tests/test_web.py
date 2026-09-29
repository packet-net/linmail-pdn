"""Webmail and the management pages through pdn's app gateway (faked here
by sending the headers pdn's gateway sets)."""

from __future__ import annotations

import re
import urllib.error
import urllib.request
import uuid

import pytest

from conftest import linmail_cfg

PREFIX = "/apps/linmail"


def get(lm, path, user=None, scope="read", gateway=True, data=None, headers=None):
    """Returns (status, body text)."""
    h = dict(headers or {})
    if gateway:
        h["X-Pdn-Gateway"] = "1"
        h["X-Forwarded-Prefix"] = PREFIX
    if user is not None:
        h["X-Pdn-User"] = user
        h["X-Pdn-Scope"] = scope
    req = urllib.request.Request(f"http://127.0.0.1:{lm.web_port}{path}", data=data, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status, r.read().decode("latin-1")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("latin-1")


def multipart(fields: dict) -> tuple[bytes, str]:
    boundary = uuid.uuid4().hex
    body = b"".join(
        f"--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"\r\n\r\n{v}\r\n".encode()
        for k, v in fields.items()) + f"--{boundary}--\r\n".encode()
    return body, f"multipart/form-data; boundary={boundary}"


@pytest.fixture
def web(linmail, fake):
    linmail.write("linmail.cfg", linmail_cfg(sysops=["N0SYS"]))
    # One webmail form template folder, so the template pages have something to list
    forms = linmail.dir / "Standard_Templates" / "General"
    forms.mkdir(parents=True)
    (forms / "Test.txt").write_text("Form: Test.html\n")
    linmail.adduser("N0USR")
    linmail.adduser("N0OTH")
    linmail.start()
    linmail.wait_stdout(r"web: listening on 127.0.0.1:%d" % linmail.web_port)
    return linmail


def test_health_needs_no_gateway(web):
    assert get(web, "/health", gateway=False) == (200, "ok\n")


def test_direct_access_refused(web):
    status, body = get(web, "/WebMail", user="N0USR", gateway=False)
    assert status == 403 and "only available through the pdn control panel" in body


def test_anonymous_refused(web):
    status, body = get(web, "/WebMail")
    assert status == 403 and "Sign in to the pdn control panel" in body


def test_webmail_for_a_callsign_user(web):
    status, body = get(web, "/WebMail", user="n0usr")
    assert status == 200
    assert "User N0USR" in body
    # Root-relative links are moved under the gateway prefix, nothing escapes to pdn's root
    links = re.findall(r'(?:href|src|action)=["\']?(/[^ >"\']*)', body)
    assert links and all(l.startswith(PREFIX + "/") for l in links), links
    web.wait_bbslog(r"Webmail Connect from N0USR through pdn \(as n0usr\)")


def test_webmail_compose_and_read(web):
    status, body = get(web, "/WebMail", user="N0USR")
    key = re.search(r"WMB\?(W[0-9A-F]+)", body).group(1)

    data, ctype = multipart({"To": "N0ABC", "Subj": "Sent from webmail", "Type": "P", "BID": "",
                             "Msg": "Hello through the gateway", "Send": "Send"})
    status, body = get(web, f"/WebMail/EMSave?{key}", user="N0USR", data=data,
                       headers={"Content-Type": ctype})
    assert status == 200
    web.wait_bbslog(r"Msg (\d+) Routing Trace To N0ABC")

    status, body = get(web, f"/WebMail/WMALL?{key}", user="N0USR")
    assert status == 200 and "Sent from webmail" in body


def test_webmail_key_is_not_shared(web):
    _, body = get(web, "/WebMail", user="N0USR")
    key = re.search(r"WMB\?(W[0-9A-F]+)", body).group(1)
    # Another signed-in user presenting that key gets their own session, not N0USR's
    status, body = get(web, f"/WebMail/WMB?{key}", user="N0OTH")
    assert status == 200 and "User N0OTH" in body and key not in body


def test_unknown_callsign_and_plain_username(web):
    status, body = get(web, "/WebMail", user="N0NEW")
    assert status == 403 and "no BBS account for N0NEW" in body
    status, body = get(web, "/WebMail", user="alice")
    assert status == 403 and "not a callsign" in body


def test_management_pages_need_admin(web):
    status, body = get(web, "/Mail/Header", user="N0USR", scope="read")
    assert status == 403 and "admin rights" in body
    status, body = get(web, "/", user="N0USR", scope="operate")
    assert status == 200 and "Mail management" not in body


def test_admin_plain_username_is_the_sysop(web):
    status, body = get(web, "/", user="admin", scope="admin")
    assert status == 200 and "BBS user N0LMB" in body and "Mail management" in body

    status, body = get(web, "/Mail/Header", user="admin", scope="admin")
    assert status == 200
    key = re.search(r"/Mail/Conf\?(M[0-9A-F]+)", body).group(1)
    for page in ("Status", "Conf", "Users", "Msgs", "FWD", "WP", "HK", "Wel"):
        status, body = get(web, f"/Mail/{page}?{key}", user="admin", scope="admin")
        assert status == 200, page
        assert "File is missing" not in body and "Wrong Version" not in body, page
        links = re.findall(r'(?:href|src|action)=["\']?(/[^ >"\']*)', body)
        assert all(l.startswith(PREFIX + "/") for l in links), (page, links)

    # Another admin can't ride that session
    status, body = get(web, f"/Mail/Conf?{key}", user="root", scope="admin")
    assert status == 200 and key not in body


def test_sysop_bbs_account_needs_admin(web):
    status, body = get(web, "/WebMail", user="N0SYS", scope="read")
    assert status == 403 and "needs admin rights" in body
    status, body = get(web, "/WebMail", user="N0SYS", scope="admin")
    assert status == 200 and "User N0SYS" in body


def webmail_key(web, user="N0USR", scope="read"):
    _, body = get(web, "/WebMail", user=user, scope=scope)
    return re.search(r"WMB\?(W[0-9A-F]+)", body).group(1)


def test_forged_host_refused(web):
    # DNS rebinding: a browser sent here under another name, with made-up X-Pdn headers
    status, body = get(web, "/WebMail", user="N0USR", headers={"Host": f"evil.example:{web.web_port}"})
    assert status == 403
    status, _ = get(web, "/WebMail", user="N0USR", headers={"Host": f"localhost:{web.web_port}"})
    assert status == 200


def test_callsign_admin_without_bbs_account(web):
    # The first-run case: the node owner (M0ABC) has no BBS account yet
    status, body = get(web, "/", user="M0ABC", scope="admin")
    assert status == 200 and "no BBS account for M0ABC" in body and "Mail management" in body
    status, body = get(web, "/Mail/Header", user="M0ABC", scope="admin")
    assert status == 200 and "/apps/linmail/Mail/Conf?" in body
    status, body = get(web, "/WebMail", user="M0ABC", scope="admin")
    assert status == 403 and "Webmail needs one" in body and "Mail management" in body
    # Without admin it is a plain refusal
    status, body = get(web, "/Mail/Header", user="M0ABC", scope="read")
    assert status == 403 and "no BBS account for M0ABC" in body


def test_percent_at_end_of_form_field(web):
    key = webmail_key(web)
    data, ctype = multipart({"To": "N0ABC", "Subj": "100%", "Type": "P", "BID": "%",
                             "Msg": "Ends in a percent sign %", "Send": "Send"})
    status, _ = get(web, f"/WebMail/EMSave?{key}", user="N0USR", data=data, headers={"Content-Type": ctype})
    assert status == 200
    web.wait_bbslog(r"Routing Trace To N0ABC")
    # The template list decodes every field (UndoTransparency)
    for body in ("Ends in a percent sign %", "Ends in %4", "%zz and %"):
        data, ctype = multipart({"To": "N0ABC%", "Subj": "100%", "Type": "P", "BID": "%", "Msg": body})
        status, _ = get(web, f"/WebMail/GetTemplates?{key}", user="N0USR", data=data, headers={"Content-Type": ctype})
        assert status == 200
    assert get(web, "/health", gateway=False)[0] == 200


def test_templates_post_without_fields(web):
    # UPSTREAM-BUGS.md 1: the template list reads BID (and To, Subj, Msg) unchecked
    key = webmail_key(web)
    data, ctype = multipart({"Type": "P"})
    status, body = get(web, f"/WebMail/GetTemplates?{key}", user="N0USR", data=data, headers={"Content-Type": ctype})
    assert status == 200 and "Select Required Template" in body
    # And compose with nothing but a type
    status, _ = get(web, f"/WebMail/EMSave?{key}", user="N0USR", data=data, headers={"Content-Type": ctype})
    assert status == 200
    assert get(web, "/health", gateway=False)[0] == 200


def test_template_folder_out_of_range(web):
    # UPSTREAM-BUGS.md 2: folder and form numbers index tables unchecked
    key = webmail_key(web)
    status, body = get(web, f"/WebMail/GetList/0?{key}", user="N0USR")
    assert status == 200 and "Test.txt" in body
    for path in ("GetList/100000", "GetList/0:5", "GetList/abc", "GetPage/100000", "GetPage/0,99", "GetPage/-1"):
        status, body = get(web, f"/WebMail/{path}?{key}", user="N0USR")
        assert status == 400, path
    assert get(web, "/health", gateway=False)[0] == 200


def test_half_closed_client_gets_its_reply(web):
    import socket
    s = socket.create_connection(("127.0.0.1", web.web_port))
    s.sendall(b"GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")
    s.shutdown(socket.SHUT_WR)
    s.settimeout(5)
    reply = b""
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        reply += chunk
    s.close()
    assert reply.startswith(b"HTTP/1.1 200") and reply.endswith(b"ok\n")
