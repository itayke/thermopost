# Cloud sink — Google Apps Script Web App

`components/cloud/` POSTs batches of temperature records as JSON to a Google
Apps Script Web App, which appends one row per record to a Sheet. No server to
run and no billing — just a script bound to a Sheet you own.

The device holds the deployment URL and a shared token in NVS, never in the
firmware source, so re-deploying the script does not mean re-flashing anything.

## What a fleet changes

Thermoposts are deployed in groups, one per tank, all writing to the **same
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

## Three tabs, and why

`readings` is the history: one row per record, deduped on `device` + `index`.

`posts` is the fleet's **current state**: one row per post, rewritten on every
batch, carrying its name, when it was last heard from, its pack voltage, whether
it is on external power, the longest it means to go between uploads (`gap_s`), and
what its newest record says about the band (`temp`: blank, `high`, `low` or
`fault`).

`config` holds the alert settings — recipients and thresholds — as key/value rows,
so they change without touching the script. See **4. Alerts**. That tab exists because the alerting question —
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
// Bound to the destination Sheet. Appends one row per posted record, keeps one row
// per post in `posts`, and emails the fleet manager when a post needs attention.
// This is the firmware's built-in default, so a fresh post syncs with only the
// /exec URL set. Recommended: replace it with your own long random string
// (<= 63 chars) and match it on each device — see step 4.
const TOKEN = 'Thermopost (c) 2026 Itay Keren';   // must match `cloud token`
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
  // Appended, like the readings columns, so an existing tab keeps its layout.
  'gap_s', 'temp',
];

// What the fleet manager tunes, in the `config` tab. Created with these defaults on
// first use; a key added by a later version of this script is appended on its own.
const CONFIG = 'config';
const CONFIG_DEFAULTS = [
  ['alert_to', '', 'comma-separated addresses; blank sends to the Sheet owner'],
  ['batt_low_pct', 20, 'alert when the pack is below this and not on external power'],
  ['batt_clear_pct', 25, 'clear only at or above this, so a pack on the line does not flap'],
  ['silent_gaps', 2, 'a post is silent after this many of its own upload gaps...'],
  ['silent_grace_min', 10, '...plus this margin'],
  ['silent_default_min', 60, 'the silence threshold for a post that does not send its gap'],
  ['remind_hours', 6, 'repeat an alert that is still open this often; 0 never repeats'],
];

// How often the fleet check runs. One of 1, 5, 10, 15 or 30 (Apps Script's choice).
const CHECK_EVERY_MIN = 5;
// How long a request waits for another to finish. Under the device's 15 s request
// timeout, so a lost race comes back as a retry rather than a hang.
const LOCK_WAIT_MS = 10000;
const MS_PER_MIN = 60 * 1000;
const MS_PER_HOUR = 60 * MS_PER_MIN;
const SEC_PER_MIN = 60;
const SUBJECT_PREFIX = '[Thermopost] ';
// Presentation only, the stored value is the full timestamp. Set on the whole column:
// a Date written by setValues() can land shown as a bare date, hiding the time of day.
const DATETIME_FORMAT = 'yyyy-mm-dd hh:mm:ss';
const STATE_PREFIX = 'alert:';

const TEMP_WORDS = { high: 'above its maximum', low: 'below its minimum', fault: 'sensor fault' };

function doPost(e) {
  // One request at a time. Two posts uploading together would otherwise both read
  // the same last row and write their batches over each other.
  const lock = LockService.getScriptLock();
  if (!lock.tryLock(LOCK_WAIT_MS)) return json_({ ok: false, error: 'busy' });
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
    const battMv = blankIfNull_(body.batt_mv);
    const battPct = blankIfNull_(body.batt_pct);
    const ext = body.ext === true;
    // The longest the post means to go between uploads. Blank from older firmware.
    const gapS = blankIfNull_(body.gap_s);
    const records = body.records || [];

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
    records.forEach(function (r) {
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
      showTimes_(sh, HEADERS.indexOf('datetime') + 1);
    }

    // Done even when every record was a duplicate: a retried batch still proves the
    // post is alive and still carries a current voltage, and a fleet view that went
    // stale because a response was lost would raise exactly the wrong alarm.
    const batch = summarize_(records);
    const post = upsertStatus_(ss, device, name, battMv, battPct, ext, gapS, batch);

    // This post's alerts now, while its batch is in hand. Silence is the timed
    // check's job, since a silent post is exactly the one that never calls this.
    try {
      checkPost_(config_(ss), post, batch, new Date());
    } catch (alertErr) {
      // A failed email (quota, a bad address) must never cost the post its upload.
      console.error('alert check failed: ' + alertErr);
    }

    return json_({ ok: true, written: rows.length });
  } catch (err) {
    return json_({ ok: false, error: String(err) });
  } finally {
    lock.releaseLock();
  }
}

// Every post, on a timer: the only way to notice one that has stopped talking, and
// what repeats an alert that is still open. Installed by installAlerts().
function checkFleet() {
  const lock = LockService.getScriptLock();
  if (!lock.tryLock(LOCK_WAIT_MS)) return;  // the next run picks it up
  try {
    const ss = SpreadsheetApp.getActiveSpreadsheet();
    const sh = ss.getSheetByName(STATUS);
    if (!sh || sh.getLastRow() < 2) return;
    const cfg = config_(ss);
    const now = new Date();
    const rows = sh.getRange(2, 1, sh.getLastRow() - 1, STATUS_HEADERS.length).getValues();
    rows.forEach(function (row) {
      if (row[0]) checkPost_(cfg, post_(row), null, now);
    });
  } finally {
    lock.releaseLock();
  }
}

// Run once from the editor: creates the config tab, grants the mail permission,
// installs the timed check, and sends a test message so delivery is proved now
// rather than during the first real alert.
function installAlerts() {
  const ss = SpreadsheetApp.getActiveSpreadsheet();
  const cfg = config_(ss);
  ScriptApp.getProjectTriggers().forEach(function (t) {
    if (t.getHandlerFunction() === 'checkFleet') ScriptApp.deleteTrigger(t);
  });
  ScriptApp.newTrigger('checkFleet').timeBased().everyMinutes(CHECK_EVERY_MIN).create();
  send_(cfg, 'alerts are on',
        'The fleet is checked every ' + CHECK_EVERY_MIN + ' minutes. Thresholds and ' +
        'recipients are in the "' + CONFIG + '" tab.');
}

// --- The posts tab ------------------------------------------------------------

// One row per device, found by device id and rewritten in place. Returns the post as
// the alert check reads it.
function upsertStatus_(ss, device, name, battMv, battPct, ext, gapS, batch) {
  let sh = ss.getSheetByName(STATUS);
  if (!sh) {
    sh = ss.insertSheet(STATUS);
    sh.appendRow(STATUS_HEADERS);
  } else if (sh.getLastColumn() < STATUS_HEADERS.length) {
    sh.getRange(1, 1, 1, STATUS_HEADERS.length).setValues([STATUS_HEADERS]);
  }

  let rowNum = 0;
  let prev = null;
  const lastRow = sh.getLastRow();
  if (lastRow > 1) {
    const rows = sh.getRange(2, 1, lastRow - 1, STATUS_HEADERS.length).getValues();
    for (let i = 0; i < rows.length; i++) {
      if (rows[i][0] === device) { rowNum = i + 2; prev = rows[i]; break; }
    }
  }
  // A batch with no records keeps what the last one said about the readings.
  const lastIndex = batch.lastIndex !== null ? batch.lastIndex : (prev ? prev[6] : '');
  const temp = batch.latest !== null ? batch.latest : (prev ? prev[8] : '');
  const row = [device, name, new Date(), battMv, battPct, ext, lastIndex, gapS, temp];

  if (rowNum) {
    sh.getRange(rowNum, 1, 1, STATUS_HEADERS.length).setValues([row]);
  } else {
    sh.appendRow(row);
  }
  showTimes_(sh, STATUS_HEADERS.indexOf('last_seen') + 1);
  return post_(row);
}

// Show a timestamp column with its time of day, existing rows included.
function showTimes_(sh, col) {
  sh.getRange(2, col, sh.getMaxRows() - 1, 1).setNumberFormat(DATETIME_FORMAT);
}

function post_(row) {
  return {
    device: row[0], name: row[1], lastSeen: row[2], battMv: row[3], battPct: row[4],
    ext: row[5] === true, gapS: row[7], temp: row[8] || '',
  };
}

// What a batch says about the band: how many records left it which way, how far, and
// what the newest one says now. `latest` is null for a batch with no records.
function summarize_(records) {
  const s = { high: 0, low: 0, fault: 0, peak: null, trough: null, latest: null, lastIndex: null };
  let newest = null;
  records.forEach(function (r) {
    if (newest === null || r.index > newest.index) newest = r;
    if (r.high) {
      s.high++;
      if (r.max !== null && (s.peak === null || r.max > s.peak)) s.peak = r.max;
    }
    if (r.low) {
      s.low++;
      if (r.min !== null && (s.trough === null || r.min < s.trough)) s.trough = r.min;
    }
    if (r.fault) s.fault++;
  });
  if (newest !== null) {
    s.latest = newest.fault ? 'fault' : newest.high ? 'high' : newest.low ? 'low' : '';
    s.lastIndex = newest.index;
  }
  s.flagged = s.high + s.low + s.fault;
  return s;
}

// --- Alerts ---------------------------------------------------------------------

// Each kind of alert is a state per post: one email when it opens, one when it clears,
// and a reminder while it stays open. Kept in script properties, out of the Sheet,
// so nobody edits it by accident.
function checkPost_(cfg, post, batch, now) {
  const label = post.name || post.device;
  const state = loadState_(post.device);
  const details = postDetails_(cfg, post);

  // Temperature: the newest record decides. A batch that left the band and came back
  // inside one upload gets a message of its own, or it would never be heard of.
  const tempOn = post.temp !== '';
  if (batch && batch.flagged > 0 && !tempOn && !(state.temp && state.temp.on)) {
    send_(cfg, label + ': left its band and came back', batchText_(batch) + details);
  }
  step_(cfg, state, 'temp', tempOn, TEMP_WORDS[post.temp] || '', label, 'back in band',
        (batch ? batchText_(batch) : '') + details, now);

  // Battery, with a gap between the two thresholds so a pack sitting on the line
  // does not open and clear every batch. Blank is no sense at all, never "flat".
  const pct = post.battPct;
  const wasLow = state.batt && state.batt.on;
  const battOn = pct !== '' && pct !== null && !post.ext &&
                 pct < (wasLow ? cfg.batt_clear_pct : cfg.batt_low_pct);
  step_(cfg, state, 'batt', battOn, 'battery low', label, 'battery recovered', details, now);

  // Silence: twice the post's own longest gap plus a margin, so a post set to upload
  // hourly is not called silent at minute forty. Any batch ends it.
  const limitMin = post.gapS
      ? cfg.silent_gaps * post.gapS / SEC_PER_MIN + cfg.silent_grace_min
      : cfg.silent_default_min;
  const silentOn = !batch && post.lastSeen instanceof Date &&
                   now - post.lastSeen > limitMin * MS_PER_MIN;
  step_(cfg, state, 'silent', silentOn, 'silent', label, 'reporting again', details, now);

  saveState_(post.device, state);
}

// `what` names the condition and must not carry a reading: a changed `what` is a new
// alert, which is how high turning to low is told apart from a reminder.
function step_(cfg, state, kind, on, what, label, clearedWhat, body, now) {
  const s = state[kind] || { on: false, what: '', sent: 0 };
  if (on && (!s.on || s.what !== what)) {
    send_(cfg, label + ': ' + what, body);
    state[kind] = { on: true, what: what, sent: now.getTime() };
  } else if (on && cfg.remind_hours > 0 &&
             now.getTime() - s.sent >= cfg.remind_hours * MS_PER_HOUR) {
    send_(cfg, label + ': still ' + what, body);
    s.sent = now.getTime();
    state[kind] = s;
  } else if (!on && s.on) {
    send_(cfg, label + ': ' + clearedWhat, body);
    state[kind] = { on: false, what: '', sent: 0 };
  }
}

function batchText_(b) {
  const parts = [];
  if (b.high) parts.push(b.high + ' above the maximum' + (b.peak !== null ? ', peak ' + b.peak + ' °C' : ''));
  if (b.low) parts.push(b.low + ' below the minimum' + (b.trough !== null ? ', low ' + b.trough + ' °C' : ''));
  if (b.fault) parts.push(b.fault + ' with no reading');
  return parts.length ? 'In the latest upload: ' + parts.join('; ') + '.\n' : '';
}

function postDetails_(cfg, post) {
  const batt = post.battPct === '' ? 'no battery sense'
      : post.battPct + '% (' + post.battMv + ' mV)' + (post.ext ? ', on external power' : '');
  return '\nLast heard from: ' + when_(cfg, post.lastSeen) +
         '\nBattery: ' + batt +
         '\nDevice: ' + post.device +
         '\n\n' + cfg.url;
}

function when_(cfg, d) {
  return d instanceof Date ? Utilities.formatDate(d, cfg.tz, 'yyyy-MM-dd HH:mm') : 'never';
}

function send_(cfg, subject, body) {
  MailApp.sendEmail(cfg.to, SUBJECT_PREFIX + subject, body);
}

function loadState_(device) {
  const raw = PropertiesService.getScriptProperties().getProperty(STATE_PREFIX + device);
  return raw ? JSON.parse(raw) : {};
}

function saveState_(device, state) {
  PropertiesService.getScriptProperties().setProperty(STATE_PREFIX + device, JSON.stringify(state));
}

// The config tab as an object, with defaults for anything blank or missing.
function config_(ss) {
  let sh = ss.getSheetByName(CONFIG);
  if (!sh) {
    sh = ss.insertSheet(CONFIG);
    sh.appendRow(['key', 'value', 'meaning']);
  }
  const cfg = {};
  const present = {};
  const vals = sh.getDataRange().getValues();
  CONFIG_DEFAULTS.forEach(function (d) { cfg[d[0]] = d[1]; });
  for (let i = 1; i < vals.length; i++) {
    const key = vals[i][0];
    present[key] = true;
    if (key in cfg && vals[i][1] !== '') cfg[key] = vals[i][1];
  }
  CONFIG_DEFAULTS.forEach(function (d) {
    if (!present[d[0]]) sh.appendRow(d);
  });
  cfg.to = cfg.alert_to || Session.getEffectiveUser().getEmail();
  cfg.tz = ss.getSpreadsheetTimeZone();
  cfg.url = ss.getUrl();
  return cfg;
}

function blankIfNull_(v) {
  return (v === null || v === undefined) ? '' : v;
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

The config form takes the `/exec` URL and the token: from the setup AP on a fresh
post, or at `http://<name>.local/` once it is on the LAN. The serial console
(`idf.py monitor`, prompt `post>`) does the same:

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

## 4. Alerts

The script emails the fleet manager when a post needs attention. Two paths feed it:

- **Every upload** checks that post at once: its newest record's place in the band,
  and its battery. A flagged record is uploaded urgently by the post, so an
  excursion reaches the inbox within one upload of happening.
- **A timed check** every few minutes walks the `posts` tab. It is the only way to
  notice a post that has stopped talking — a silent post never calls `doPost` — and
  it is what sends reminders for anything still open.

| alert | opens when | clears when |
|---|---|---|
| above its maximum / below its minimum / sensor fault | the newest record is flagged | the newest record is back in band |
| left its band and came back | an upload holds flagged records but ends in band | — one message, nothing to clear |
| battery low | `batt_pct` below `batt_low_pct` and not on external power | at or above `batt_clear_pct`, or on external power |
| silent | nothing heard for `silent_gaps` × the post's `gap_s`, plus `silent_grace_min` | the next upload |

**One email when an alert opens, one when it clears**, and a reminder every
`remind_hours` while it stays open. A post going from above its maximum to below
its minimum is a new alert, not a reminder. The open/closed state lives in the
script's properties rather than the Sheet, so nobody edits it by accident.

**Silence is measured against each post's own cadence.** Every batch carries
`gap_s`, the longest the post means to go between uploads — the longer of its two
upload cadences, or its record interval when both are 0. A fixed threshold would
fire all day on a post set to upload hourly. Posts on firmware too old to send it
fall back to `silent_default_min`. A post switched off with **Turn Off** goes
silent too, which is the honest reading — nobody can tell off from dead from out
of range.

**Battery alerts ignore posts that cannot measure themselves.** A blank `batt_pct`
is no sense at all, not a flat pack.

To turn alerts on:

1. Paste the script above over the old one and **Deploy ▸ Manage deployments ▸
   edit ▸ Version: New version**. Editing the existing deployment keeps the `/exec`
   URL; **New deployment** would mint another, and every post would need it set.
2. In the editor, pick **`installAlerts`** and **Run**. It asks for permission to send
   mail as you, creates the `config` tab, installs the timed check, and sends a test
   email — so delivery is proved now rather than during the first real alarm.
3. Edit `config` to taste. A blank `alert_to` sends to the Sheet's owner.

Run `installAlerts` again after changing `CHECK_EVERY_MIN`; it replaces the old
trigger rather than adding a second.

**Email quota.** A consumer Google account sends about 100 emails a day, a Workspace
account about 1,500. One email per change of state keeps a fleet far under that;
`remind_hours` is the setting that spends it.

## Notes / gotchas

- **Uploads take turns.** `doPost` holds a script lock for its whole run. Two posts
  uploading at the same moment would otherwise both read the same last row and
  write their batches over each other. A post that waits too long gets `busy`, and
  retries like any other failed upload.
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
