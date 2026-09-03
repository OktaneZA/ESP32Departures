# Security

Departure Buddy flashes firmware from a web page, ships an unsigned executable,
and handles your WiFi password. This is what it does about that, and — just as
usefully — what it deliberately does not.

## Reporting something

Open an issue at
[github.com/OktaneZA/ESP32Departures/issues](https://github.com/OktaneZA/ESP32Departures/issues).
This is a hobby project with no bounty and no SLA. If you find something that
puts users at real risk, say so plainly in the title and I will prioritise it.

---

## Check what your board is running

The board will tell you the checksum of its own firmware, so you never have to
take the published binary on trust.

Over USB, send `HASH` and it answers:

```
md5=8990ce38a00dfad263509621045461ad
size=1054192
END
```

Compare that with the `md5` for `firmware.bin` in the release manifest at
`/firmware/manifest.json`, or check the file yourself:

```bash
md5sum firmware.bin
```

The setup page does this automatically after connecting and tells you
**"Verified: running the published firmware"**, or warns you if it does not
match. A mismatch is expected if you built it yourself or flashed an older
release — and worth investigating if you did neither.

This works because `ESP.getSketchMD5()` hashes exactly the image length written
by the flasher, which is precisely the contents of `firmware.bin`.

## Verified downloads

| Artefact | How to check it |
|---|---|
| `firmware.bin` and friends | SHA-256 per file in `/firmware/manifest.json`. The browser flasher verifies every image **before writing a byte** — a truncated download is refused rather than half-flashed onto a board |
| `DepartureBuddyInstaller.exe` | SHA-256 published beside it on the release |
| `web/vendor/esptool.js` | SHA-256 recorded in `web/vendor/README.md` and **enforced in CI** — the deploy fails if the vendored copy drifts |

## Network

- **TLS is verified**, not merely encrypted. All four feeds check the server
  against Mozilla's root store, embedded from
  `data/cert/x509_crt_bundle.bin` (regenerate with `docs/gen_crt_bundle.py`).
  Building with `-DTLS_INSECURE` disables that; it exists only to diagnose a
  broken chain and is never how a release is built.
- **No inbound anything.** The board listens on no port and runs no server. It
  polls a handful of APIs and draws the result.
- **Polling is rate-limited by design**, each feed at an interval matched to its
  own upstream cache — 30s for TfL buses, 15 minutes for weather. Being a good
  citizen of free APIs is a security property too: the fastest way to lose
  access is to look like an attack.
- **A metered feed is paced by its allowance, not by a guess.** Where a provider
  sells requests per day, the board is given the number and derives its own
  interval: the allowance spread evenly across the hours the screen is on, with
  nothing spent overnight and failed attempts counted against it like any other.
  A board cannot exceed a quota it was told about, however badly the network
  behaves.

## Secrets

- Your WiFi password and API keys are stored in the board's NVS and **never
  reported back**: `GET` returns the password only as `passlen`, the rail API key
  not at all, and the TransportAPI `app_key` only as `buskeylen`. The matching
  `app_id` *is* reported, deliberately — on its own it grants nothing, and
  seeing it is how you tell which account a board is spending the quota of.
- **Credentials never travel to an unverified server.** The TransportAPI request
  carries a key in its query string, so that client verifies TLS like the rest
  *and* refuses to follow redirects — the canonical URL is pinned, and a
  redirect would hand the key to wherever it pointed.
- The setup page is **static and runs entirely in your browser**. What you type
  goes down the USB cable, not to a server. There is no analytics, no telemetry
  and no backend to leak.
- **One documented exception:** the settings file you can download contains your
  WiFi password in plain text, so the installer can set the board up without you
  retyping it. The page says so at the point of download, and the installer
  offers to delete the file afterwards.

## What is *not* protected

Being explicit is more useful than a clean-looking list:

- **The executable is unsigned.** Windows SmartScreen will warn about it. A
  code-signing certificate costs more per year than the hardware; the published
  SHA-256 is the alternative. Verify it if you care.
- **No secure boot or flash encryption.** Anyone with physical access and a USB
  cable can read the firmware and reflash the board. For a desk ornament showing
  public timetable data this is the right trade — enabling secure boot would
  make the browser installer impossible.
- **The board trusts its APIs.** Responses are parsed with bounded buffers and
  size caps, so a malformed reply cannot exhaust the heap, but a compromised
  upstream could still show you a wrong departure time.
- **Anyone with the board can reconfigure it** over USB. There is no PIN on the
  serial protocol. Physical access is total access.

## Supply chain

- Every GitHub Action is **pinned to a commit SHA**, not a tag. The sharpest
  edge was `softprops/action-gh-release`, third-party and running with
  `contents: write` — a moved tag could otherwise publish arbitrary releases.
- The vendored `esptool.js` is byte-identical to upstream, with its provenance
  and a re-verification command in `web/vendor/README.md`.
- Release firmware is **compiled from the tagged source in the same job** that
  builds the installer, so an installer can never ship a firmware image from a
  different commit.
