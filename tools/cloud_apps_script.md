# Cloud sink — Google Apps Script Web App

`components/cloud/` POSTs batches of temperature records as JSON to a Google
Apps Script Web App, which appends one row per record to a Sheet. No server to
run and no billing — just a script bound to a Sheet you own.

The device holds the deployment URL and a shared token in NVS, never in the
firmware source, so re-deploying the script does not mean re-flashing anything.

## What a fleet changes

Thermo Posts are deployed in groups, one per tank, all writing to the **same
Sheet**. That has one consequence worth understanding before the code below
makes sense:

**The dedupe key is `device` + `index`, not `index` alone.** Every post numbers
its own records from zero, so post A's record 41 and post B's record 41 are
different readings that would collide on the first day. And the post's *name*
cannot stand in for the device id either — it is renameable from the config form
by design, and a key that changes when someone fixes a typo is not a key. So:

| field | what it is | changes? |
|---|---|---|
| `device` | the board's MAC as lowercase hex | never |
| `name` | what a human calls this post | any time, from the form |
| `index` | the record's absolute position in *that post's* log | never reused |

`name` is written to every row so the Sheet can be filtered and sorted by tank.
A rename retags the queued backlog too, because records are labelled at upload
rather than at write — which is the behaviour you want: renaming a post should
relabel that post, not leave a seam in the middle of its history.

## Two tabs, and why

`readings` is the history: one row per record, deduped on `device` + `index`.

`posts` is the fleet's **current state**: one row per post, rewritten on every
batch, carrying its name, when it was last heard from, its pack voltage and
whether it is on external power. That tab exists because the alerting question —
"which posts need a visit?" — is a scan of a handful of rows there rather than of
a hundred thousand in `readings`. A conditional-format rule or a time-driven
trigger over `posts` is a few lines; the same thing over the history is not.

It is updated even when every record in the batch was a duplicate. A retried
batch still proves the post is alive and still carries a current voltage, and a
fleet view that went stale because one response was lost would raise exactly the
wrong alarm.

**The battery fields describe the post, not the reading.** They ride in the batch
envelope rather than in each record, so every row of one batch repeats the same
value. That is deliberate on the device side — a record is fixed at 16 bytes so
absolute indices can never shift, and the pack barely moves inside one interval
anyway. Chart the voltage from `posts`, or from one row per batch; row by row it
will look like a step function, because it is one.

`batt_mv` and `batt_pct` are **blank when the post has no usable sense** — no
divider fitted, or an ADC that did not answer. Blank is not zero: a flat pack
reports `0`, and a post that cannot measure itself reports nothing. Alerting on
`batt_pct < 20` must not also fire for every post that never had a battery.

**`ext` is what stops the alert being noise.** A post at 3.6 V and charging is
recovering; a post at 3.6 V and discharging needs a visit. The voltage alone
cannot tell them apart, so a useful rule is `batt_pct < 20 AND ext = FALSE`.

## 1. Create the Sheet + script

1. New Google Sheet — this becomes the log.
2. **Extensions ▸ Apps Script** opens a bound script project.
3. Replace `Code.gs` with the script below.
4. Leave `TOKEN` at the device default if you just want it working, or —
   **recommended for real access control** — change it to a long random string
   and set the matching value on every post with `cloud token`. Under 63
   characters (the device's field width).

```js
// Bound to the destination Sheet. Appends one row per posted record.
// This is the firmware's built-in default, so a fresh post syncs with only the
// /exec URL set. Recommended: replace it with your own long random string
// (<= 63 chars) and match it on each device — see step 4.
const TOKEN = 'Thermo Post (c) 2026 Itay Keren';   // must match `cloud token`
const SHEET = 'readings';

// Column order. `device` + `index` together are the dedupe key: a batch retried
// after its response was lost (the device only advances its high-water mark on a
// confirmed ok) must not write the same reading twice.
const HEADERS = [
  'device', 'name', 'index', 'epoch', 'datetime', 'uptime_s',
  'mean_c', 'min_c', 'max_c', 'high', 'low', 'fault', 'v',
  // Appended, never inserted: the dedupe read below addresses columns 1-3 by
  // number, and an existing Sheet's rows would all shift under their headers.
  'batt_mv', 'batt_pct', 'ext',
];

// One row per post, overwritten every batch — the fleet's current state, which is
// what you actually alert on. Scanning 100k reading rows to find each post's latest
// battery is the query you do not want to write.
const STATUS = 'posts';
const STATUS_HEADERS = [
  'device', 'name', 'last_seen', 'batt_mv', 'batt_pct', 'ext', 'last_index',
];

function doPost(e) {
  try {
    const body = JSON.parse(e.postData.contents);
    if (body.token !== TOKEN) {
      return json_({ ok: false, error: 'bad token' });
    }
    const device = String(body.device || '');
    const name = String(body.name || '');
    if (!device) {
      return json_({ ok: false, error: 'missing device id' });
    }

    // Envelope-level, so they describe the POST rather than any reading in it: the
    // pack barely moves in a minute, and a record is nailed to 16 bytes on the
    // device precisely so absolute indices never shift. null means the post has no
    // usable sense — no divider, or an ADC that did not answer — which is a
    // different thing from a flat pack, and blank keeps it that way in the Sheet.
    const battMv = (body.batt_mv === null || body.batt_mv === undefined) ? '' : body.batt_mv;
    const battPct = (body.batt_pct === null || body.batt_pct === undefined) ? '' : body.batt_pct;
    const ext = body.ext === true;

    const ss = SpreadsheetApp.getActiveSpreadsheet();
    let sh = ss.getSheetByName(SHEET);
    if (!sh) {
      sh = ss.insertSheet(SHEET);
      sh.appendRow(HEADERS);
    } else if (sh.getLastColumn() < HEADERS.length) {
      // A Sheet created before the battery columns existed. Widening the header row
      // is the whole migration — old rows simply have those cells blank, which is
      // the honest reading of "this post never reported it".
      sh.getRange(1, 1, 1, HEADERS.length).setValues([HEADERS]);
    }

    // Existing device+index pairs, for an idempotent write. Only the two key
    // columns are read, not the whole sheet.
    const lastRow = sh.getLastRow();
    const seen = {};
    if (lastRow > 1) {
      const keys = sh.getRange(2, 1, lastRow - 1, 3).getValues();
      for (let i = 0; i < keys.length; i++) {
        seen[keys[i][0] + '#' + keys[i][2]] = true;
      }
    }

    // Built as one array and written with a single setValues(). appendRow() in a
    // loop costs a round trip per row, which at 60 records a batch is the
    // difference between a second and a minute.
    const rows = [];
    (body.records || []).forEach(function (r) {
      const k = device + '#' + r.index;
      if (seen[k]) return;
      seen[k] = true;
      // A real Date, not a string, so it sorts and charts natively; the sheet
      // renders it in its own timezone. Blank when the record predates the first
      // SNTP sync — blank rather than 1970, with uptime_s as the ordering
      // fallback. `epoch` stays the unambiguous UTC source of truth.
      const dt = r.epoch ? new Date(r.epoch * 1000) : '';
      rows.push([
        device, name, r.index, r.epoch || '', dt, r.up,
        r.c, r.min, r.max, r.high, r.low, r.fault, r.v,
        // Repeated on every row of the batch, because that is what the envelope
        // means: the post's state when it sent them, not a per-reading measurement.
        // Chart it from the Posts tab, or from one row per batch, not row by row.
        battMv, battPct, ext,
      ]);
    });

    if (rows.length) {
      sh.getRange(sh.getLastRow() + 1, 1, rows.length, HEADERS.length).setValues(rows);
    }

    // Done even when every record was a duplicate: a retried batch still proves the
    // post is alive and still carries a current voltage, and a fleet view that went
    // stale because a response was lost would raise exactly the wrong alarm.
    upsertStatus_(ss, device, name, battMv, battPct, ext, body.records || []);

    return json_({ ok: true, written: rows.length });
  } catch (err) {
    return json_({ ok: false, error: String(err) });
  }
}

// One row per device in the Posts tab, found by device id and rewritten in place.
function upsertStatus_(ss, device, name, battMv, battPct, ext, records) {
  let sh = ss.getSheetByName(STATUS);
  if (!sh) {
    sh = ss.insertSheet(STATUS);
    sh.appendRow(STATUS_HEADERS);
  }
  let lastIndex = '';
  for (let i = 0; i < records.length; i++) {
    if (lastIndex === '' || records[i].index > lastIndex) lastIndex = records[i].index;
  }
  const row = [device, name, new Date(), battMv, battPct, ext, lastIndex];

  const lastRow = sh.getLastRow();
  if (lastRow > 1) {
    const ids = sh.getRange(2, 1, lastRow - 1, 1).getValues();
    for (let i = 0; i < ids.length; i++) {
      if (ids[i][0] === device) {
        sh.getRange(i + 2, 1, 1, STATUS_HEADERS.length).setValues([row]);
        return;
      }
    }
  }
  sh.appendRow(row);
}

function json_(obj) {
  return ContentService.createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}
```

## 2. Deploy as a Web App

1. **Deploy ▸ New deployment ▸** type **Web app**.
2. **Execute as: Me**, so the script can write your Sheet.
3. **Who has access: Anyone** — the device sends no Google credentials, and the
   `TOKEN` check is what gates writes.
4. Copy the **`/exec` URL** (`https://script.google.com/macros/s/AKfy.../exec`).

Re-deploying after a code change can mint a new `/exec` URL. That is exactly why
the URL lives in NVS rather than in firmware.

## 3. Point a post at it

The captive-portal form is the eventual answer and is still being built; today it
is the serial console (`idf.py monitor`, prompt `post>`):

```
cloud wifi "Brewery 2G" hunter2
cloud url https://script.google.com/macros/s/AKfy.../exec
cloud token PUT-A-LONG-RANDOM-STRING-HERE
cloud status      # link, clock, device id, queue depth; the token is never echoed
cloud sync        # try an upload now instead of waiting for the next record
cloud resync      # re-offer every record still on disk (see below)
```

`cloud status` prints the `device` value this post will write, which is how you
match a row in the Sheet back to a box on a wall.

If you clear or recreate the Sheet, a post will not re-send its history on its
own: it tracks a high-water mark in NVS and only uploads above it. `cloud resync`
rewinds that mark, and the `device`+`index` dedupe means any rows that survived
will not be duplicated.

## Notes / gotchas

- **Why a token and not auth.** A Web App set to "Anyone" needs no OAuth from a
  device that has no browser, so the shared token is the access control. It is a
  soft gate: it ships in the firmware and crosses the wire in the clear. Rotate
  it by editing `TOKEN`, redeploying, and running `cloud token` on each post.
- **The 302 redirect.** Apps Script answers `/exec` with a 302 to
  `script.googleusercontent.com`, which serves `doPost`'s return value and
  accepts GET only. `doPost` has already run, with the body, on the first hop.
  The firmware follows that redirect **manually, as a GET** — the HTTP client's
  automatic redirect preserves the POST method and gets a 405, which would make
  a write that in fact succeeded look like a failure and be retried.
- **The status code is always 200.** ContentService cannot set one, so the
  device counts a batch as uploaded only when the body contains `"ok":true`. A
  rejected token comes back as a healthy-looking 200; the high-water mark stays
  put and the device retries with backoff.
- **`fault` rows have blank temperatures.** A record whose interval produced no
  valid reading sends `null` for all three temperature fields rather than a
  sentinel, so the cell is genuinely empty and a chart skips it instead of
  plotting -327.68 °C.
- **Dedupe cost grows with the sheet.** Every POST reads the three key columns of
  every existing row. That is fine into the hundreds of thousands of rows and
  then is not; archive to a second sheet when it gets there.
- **Quotas.** Apps Script has daily runtime limits and a real cold-start latency
  — the device allows 15 s per request and batches up to 60 records per POST,
  which is an hour of history at the default one-minute cadence.
