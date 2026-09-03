"""Build the root-certificate bundle the firmware verifies TLS against.

    python docs/gen_crt_bundle.py

Downloads Mozilla's root store (as published by the curl project) and writes
data/cert/x509_crt_bundle.bin, which platformio.ini embeds and the API clients
attach with setCACertBundle().

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


def fetch(url):
    print(f"  fetching {url}")
    ctx = ssl.create_default_context()
    with urllib.request.urlopen(url, timeout=120, context=ctx) as r:
        return r.read()


def main():
    pem = fetch(SOURCE)
    certs = x509.load_pem_x509_certificates(pem)
    print(f"  {len(certs)} root certificates")

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
