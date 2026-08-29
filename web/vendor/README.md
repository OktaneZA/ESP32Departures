# Vendored dependencies

## esptool.js

Espressif's official JavaScript port of `esptool`, used to flash the board from
the browser over Web Serial. This is the same library ESP Web Tools uses.

| | |
|---|---|
| Package | [`esptool-js`](https://github.com/espressif/esptool-js) |
| Version | 0.6.1 |
| Licence | Apache-2.0 |
| Source | `https://unpkg.com/esptool-js@0.6.1/bundle.js` |
| SHA-256 | `ef7d5a237d3f273ecf546bcee65dddad90bd82cf02f22a980d1537e0cd79a152` |

The file is **byte-identical to upstream** — no local edits — so it can be
re-verified at any time:

```bash
curl -sL https://unpkg.com/esptool-js@0.6.1/bundle.js | sha256sum
```

It is vendored rather than loaded from a CDN so the configurator keeps working
if unpkg is unreachable, and so nothing third-party is fetched at runtime by a
page that handles WiFi passwords.
