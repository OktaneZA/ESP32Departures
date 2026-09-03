# Getting your bus data key (outside London)

Departure Buddy needs a key to show **bus** departures anywhere outside London.
It comes from TransportAPI and takes about five minutes.

**London buses need no key at all.** TfL publishes live arrivals openly, so if
your stop is inside London you can skip this entire page — choose *London* in
the setup page and you are done.

Trains have their own, separate key. See [api-key.md](api-key.md).

---

## Why a key is needed at all

There is no free, keyless, per-stop bus arrivals API for the UK outside London.
The Department for Transport's own **BODS** feed publishes vehicle *positions*
in bulk, not "what is next at this stop" — turning one into the other needs
timetable matching, which is a server's job rather than a small board's.
TransportAPI is the one source that answers the question directly.

---

## 1. Create an account

Go to **[developer.transportapi.com/signup](https://developer.transportapi.com/signup)**.

You can register with GitHub, or fill in the form. It asks for a company name —
put **Personal** if it is for your own board. It also wants a phone number, a
primary use case and a short description.

> The password rules are stricter than most: **at least 15 characters**, with
> upper and lower case, a digit, and a special character.

No payment card is needed for the free plan.

---

## 2. Find your credentials

Access happens through **apps**, and your account starts with one. Open it in
the developer portal and you will find two values:

| Value | Looks like | Secret? |
|---|---|---|
| `app_id` | `a1b2c3d4` — 8 hex characters | No — it identifies the account, and grants nothing on its own |
| `app_key` | 32 hex characters | **Yes.** Treat it like a password |

Both go into the setup page. Press **Check these work** and it will tell you
straight away whether they are accepted — the check runs in your browser against
a known stop, so nothing is stored anywhere and no hardware is involved.

---

## 3. Choose the right allowance

This is the part worth reading properly, because it decides how good your bus
screen will be.

TransportAPI meters by the **day**, not by the second:

| Plan | Allowance | Cost |
|---|---|---|
| Free | **30 requests a day** | free, no expiry |
| Home use | 300 a day | £5/month inc. VAT, plus a £10 setup fee |
| Commercial trial | 1000 a day | free, 30 days, non-renewable |

The board spreads whatever you have evenly across the hours your screen is
actually on, and spends nothing overnight. On a board that is on from 06:00 to
22:00 that works out as:

| Plan | One update every |
|---|---|
| Free (30/day) | **32 minutes** |
| Home use (300/day) | 3.2 minutes |
| Commercial trial (1000/day) | 1 minute |

**Be honest with yourself about the free tier.** Bus arrivals live in a 0–30
minute window, so at a 32-minute refresh a bus can comfortably appear and depart
between two updates — the screen will be right when it polls and misleading for
a while afterwards. It works, and it costs nothing, but the Home plan is what
makes the bus screen genuinely useful.

Tell the setup page which plan you are on. It only needs the number so it can
pace itself; it never checks, and setting it higher than your real allowance
just means TransportAPI starts refusing requests partway through the day.

> Older guides — and some of TransportAPI's own blog posts — mention a free tier
> of 1000 requests a day. That was reduced to 30 in September 2021, and existing
> accounts were reduced with it.

---

## 4. Pick your stop

Back in the setup page, search by postcode, town or village name, and choose
from the list. Stops come from OpenStreetMap, which carries the **ATCO code**
that TransportAPI indexes by, so you never have to look one up yourself.

If a stop you can see in real life is missing from the list, it is almost always
because OpenStreetMap has no ATCO code recorded for it. A stop on the other side
of the same road usually works.

---

## What the board does with it

Your `app_key` is stored on the board and is **never reported back** — `GET`
returns only its length, exactly as it does for your WiFi password. The board
talks to TransportAPI over TLS verified against Mozilla's root store, so the
credentials cannot be handed to an impostor server, and it never follows a
redirect that might carry them somewhere else.

See [SECURITY.md](../SECURITY.md) for the full picture.
