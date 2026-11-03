#!/usr/bin/env python3
# Starts `andromeda-index serve` on a temp home with no index: it must crawl, answer on its socket, find a file
# written after start within 1 s, and find words inside text, Office, EPUB, HTML and PDF files and in photo and audio
# metadata within 5 s.
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import zipfile

exe = sys.argv[1]
base = tempfile.mkdtemp(prefix="serve-test-")
home = os.path.join(base, "home")
env = dict(os.environ, HOME=home, XDG_STATE_HOME=os.path.join(base, "state"),
           XDG_RUNTIME_DIR=os.path.join(base, "run"))
os.makedirs(os.path.join(home, "Documents"))
os.makedirs(env["XDG_RUNTIME_DIR"])
open(os.path.join(home, "Documents", "quarterly-budget.ods"), "w").close()
docs = os.path.join(home, "Documents")


def archive(name, entries):
    with zipfile.ZipFile(os.path.join(docs, name), "w") as z:
        for entry, body in entries.items():
            z.writestr(entry, body)


archive("report.docx", {"word/document.xml": "<w:document><w:body><w:p><w:t>pangolin</w:t></w:p></w:body></w:document>"})
archive("minutes.odt", {"content.xml": "<office:text><text:p>narwhal&amp;co</text:p></office:text>"})
archive("novel.epub", {"OEBPS/ch1.xhtml": "<html><body><p>axolotl</p></body></html>"})
with open(os.path.join(docs, "page.html"), "w") as f:
    f.write("<html><script>var scriptonly = 1;</script><body><p>Caf\u00e9 tapir</p></body></html>")


def minimal_pdf(text):
    stream = f"BT /F1 12 Tf 72 712 Td ({text}) Tj ET".encode()
    objs = [b"<< /Type /Catalog /Pages 2 0 R >>", b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>",
            b"<< /Length %d >>\nstream\n" % len(stream) + stream + b"\nendstream",
            b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"]
    out, offsets = b"%PDF-1.4\n", []
    for i, body in enumerate(objs, 1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1) + b"".join(b"%010d 00000 n \n" % o for o in offsets)
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
    return out


with open(os.path.join(docs, "wide.txt"), "wb") as f:
    f.write("notes about the marmoset, saved as UTF-16 without a byte order mark\n".encode("utf-16-le"))
with open(os.path.join(docs, "scan.pdf"), "wb") as f:
    f.write(minimal_pdf("wombat"))
# Photo and audio metadata: a JPEG with an EXIF camera model and a WAV with title and artist tags.
media = []
if shutil.which("ffmpeg") and shutil.which("exiv2"):
    quiet = dict(stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    photo = os.path.join(docs, "holiday.jpg")
    subprocess.run(["ffmpeg", "-f", "lavfi", "-i", "color=c=blue:s=8x8", "-frames:v", "1", photo], **quiet)
    subprocess.run(["exiv2", "-M", "set Exif.Image.Model Ascii Okapicam", photo], **quiet)
    song = os.path.join(docs, "track01.wav")
    subprocess.run(["ffmpeg", "-f", "lavfi", "-i", "anullsrc=r=8000:cl=mono", "-t", "0.1",
                    "-metadata", "title=Narwhal Lullaby", "-metadata", "artist=Quetzal Choir", song], **quiet)
    media = [("okapicam", "holiday.jpg"), ("quetzal", "track01.wav")]
else:
    print("ffmpeg or exiv2 missing: photo and audio metadata not checked")
sock_path = os.path.join(env["XDG_RUNTIME_DIR"], "andromeda-index.sock")
failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


def ask(conn, line):
    conn.sendall(line.encode() + b"\n")
    data = b""
    while not data.endswith(b"\n\n") and data != b"\n":
        chunk = conn.recv(65536)
        if not chunk:
            break
        data += chunk
    return [r.split("\t") for r in data.decode().split("\n") if r]


daemon = subprocess.Popen([exe, "serve"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    deadline = time.monotonic() + 10
    while not os.path.exists(sock_path) and time.monotonic() < deadline:
        time.sleep(0.02)
    conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    conn.settimeout(5)
    conn.connect(sock_path)

    def found_by_text(word, name, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if any(r[0].endswith(name) and r[4] == "c" for r in ask(conn, f"query 20 {word}")):
                return True
            time.sleep(0.05)
        return False

    for word, name in [("pangolin", "report.docx"), ("narwhal", "minutes.odt"), ("axolotl", "novel.epub"),
                       ("wombat", "scan.pdf"), ("cafe tap", "page.html"), ("marmoset", "wide.txt")] + media:
        check(found_by_text(word, name, 5), f"{name} is found by a word inside")
    check(not found_by_text("scriptonly", "page.html", 0.2), "script text is not indexed")

    rows = ask(conn, "query 20 budget")
    check([r[0] for r in rows] == [os.path.join(home, "Documents", "quarterly-budget.ods")], "crawled file is found")
    check(len(rows) == 1 and rows[0][2:] == ["0", "f", "n"], "row has size, kind and match")
    check(ask(conn, "nonsense") == [], "unknown request gets an empty answer")

    open(os.path.join(home, "Documents", "fresh-invoice.pdf"), "w").close()
    written = time.monotonic()
    found = False
    while not found and time.monotonic() - written < 1.0:
        found = any(r[0].endswith("fresh-invoice.pdf") for r in ask(conn, "query 20 invoice"))
        time.sleep(0.02)
    check(found, "a new file is found within 1 s")

    with open(os.path.join(docs, "later.txt"), "w") as f:
        f.write("the quokkaberry harvest\n")
    check(found_by_text("quokkaberry", "later.txt", 5), "a new file is found by a word inside within 5 s")
    conn.close()
finally:
    daemon.terminate()
    daemon.wait()
    shutil.rmtree(base)

print("PASS" if not failures else f"{len(failures)} failures")
sys.exit(1 if failures else 0)
