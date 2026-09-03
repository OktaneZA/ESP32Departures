"""Build the root-certificate bundle the firmware verifies TLS against.

    python docs/gen_crt_bundle.py

Downloads Mozilla's root store (as published by the curl project), merges
docs/extra_roots.pem, and writes data/cert/x509_crt_bundle.bin, which
platformio.ini embeds and the API clients attach with setCACertBundle().

The supplementary file exists because esp_crt_bundle.c only ever looks up the
issuer of the *topmost* certificate a server sends. Where a server presents a
root cross-signed by an older one - as TfL and the Rail Data Marketplace both do
- that older root has to be present even though a browser would have stopped a
link earlier. Two of the five feeds fail the handshake without it.

Without this the clients call setInsecure(): traffic is encrypted but the server
is never authenticated, so anything able to intercept the connection can feed
the board whatever departures it likes.

This is a small reimplementation of ESP-IDF's own gen_crt_bundle.py, which needs
rich_click and a checkout of ESP-IDF. The format is short enough that depending
on neither is worth the fifty lines. It is parsed by esp_crt_bundle.c:

    uint16  number of certificates, big endian
    then, per certificate, sorted by DER subject:
        uint16  subject length, big endian
        uint16  public key length, big endian
        bytes   subject, DER
        bytes   SubjectPublicKeyInfo, DER

Only the subject and public key are stored, not the whole certificate: that is
all mbedTLS needs to check a signature, and it keeps the bundle to ~65KB.
"""

import os
import ssl
import struct
import sys
import urllib.request

from cryptography import x509
from cryptography.hazmat.primitives import serialization

SOURCE = "https://curl.se/ca/cacert.pem"
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "data", "cert", "x509_crt_bundle.bin")

# Roots that Mozilla has retired but that our feeds still chain through. See the
# file itself for why each one is there; without it two of the five feeds cannot
# complete a handshake at all.
EXTRA = os.path.join(HERE, "extra_roots.pem")


def fetch(url):
    print(f"  fetching {url}")
    ctx = ssl.create_default_context()
    with urllib.request.urlopen(url, timeout=120, context=ctx) as r:
        return r.read()


def main():
    pem = fetch(SOURCE)
    certs = x509.load_pem_x509_certificates(pem)
    print(f"  {len(certs)} root certificates from Mozilla")

    if os.path.exists(EXTRA):
        with open(EXTRA, "rb") as f:
            extra = x509.load_pem_x509_certificates(f.read())
        # Skip any that Mozilla has since restored, so the bundle never holds a
        # subject twice - the runtime does a binary search and a duplicate would
        # make which one it finds a matter of luck.
        have = {c.subject.public_bytes() for c in certs}
        extra = [c for c in extra if c.subject.public_bytes() not in have]
        for c in extra:
            print(f"  + supplementary: {c.subject.rfc4514_string()}")
        certs += extra
    else:
        print(f"  (no {os.path.basename(EXTRA)} - skipping supplementary roots)")

    entries = []
    skipped = 0
    for cert in certs:
        try:
            subject = cert.subject.public_bytes()
            key = cert.public_key().public_bytes(
                encoding=serialization.Encoding.DER,
                format=serialization.PublicFormat.SubjectPublicKeyInfo,
            )
        except Exception as e:                      # unsupported key type
            skipped += 1
            print(f"  skipped one certificate: {e}")
            continue
        entries.append((subject, key))

    # esp_crt_bundle.c binary-searches by subject, so the order is load-bearing
    # rather than cosmetic.
    entries.sort(key=lambda e: e[0])

    blob = struct.pack(">H", len(entries))
    for subject, key in entries:
        blob += struct.pack(">HH", len(subject), len(key)) + subject + key

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(blob)

    print(f"  skipped {skipped}" if skipped else "  skipped none")
    print(f"\nwrote {os.path.normpath(OUT)}  ({len(blob):,} bytes, {len(entries)} certificates)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
