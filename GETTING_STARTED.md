# Thermopost: Getting Started

Thermopost watches a tank's surface temperature, beeps when it leaves its safe range,
and sends every reading to a Google Sheet.

## 1. Turning it on and charging

- **Plug in the USB-C cable** and the post starts by itself. The cable also charges its battery.
- **Unplugged**, it keeps running on its battery. The battery icon at the top left shows how full it is, and a lightning bolt means it is charging.
- **The screen dims and goes dark** when nobody touches it, to save power. Tap it to wake it. It keeps measuring the whole time.
- **To switch it off:** Menu › Power › **Turn Off**, then tap again when it asks "Sure?". **To switch it back on, touch the screen.**

## 2. Reading the screen

The big number is the temperature right now. Its colour tells you how things are at a glance:

| Colour | Meaning |
| --- | --- |
| White | Inside the safe range |
| Orange-red | Too hot |
| Blue | Too cold |
| Amber | The sensor isn't answering |

- **Tap the number** to switch between °C and °F.
- **Top right:** the safe range, ↑ is the highest allowed and ↓ the lowest.
- **The graph** shows the last couple of hours, with the two limits as dashed lines.
- **Bottom:** the post's name, and how many readings it has saved and sent to the Sheet.

## 3. When it alarms

When the temperature leaves the safe range, the post beeps and shows **TOO HOT**,
**TOO COLD** or **NO READING** above the graph. It also emails the people on the
Sheet's alert list.

| Button | What it does | Press it when |
| --- | --- | --- |
| **Snooze** | Quiet for 5 minutes, then beeps again if it's still out of range | You're already dealing with it |
| **Off** | Quiet until the temperature recovers or goes out the other way | You've decided this one is fine |

Either way, the number stays coloured until the temperature is back in range. To
silence all beeping, go to Menu › Sound; the alarm still shows on screen and still
emails.

## 4. Changing settings on the post

Tap **Menu** at the top left. **Back** in the same corner always returns you.

| Page | What you can change |
| --- | --- |
| Temperature | The safe range ("Alarm below", "Alarm above"), and °C or °F |
| Recording | How often a reading is saved, and how often it's sent to the Sheet |
| Sound | Alarm sound on or off, volume, and a button to test the beep |
| Power | When the screen sleeps, the battery level, and Turn Off |
| Network | Wi-Fi status, and joining a new network (section 5) |

Change a value with the **−** and **+** buttons; hold one down to move faster.
Changes save by themselves.

If the alarm limits have **grey buttons that don't respond**, the range is being set
from the Google Sheet, and the page says so at the bottom. Change it in the Sheet
instead (section 6).

## 5. Connecting it to Wi-Fi

The post needs Wi-Fi to send readings to the Sheet. It only works with **2.4 GHz**
networks; most home and office routers offer one.

1. On the post, go to Menu › Network › **Configure**. A QR code appears.
2. Point your phone's camera at it and tap the prompt to join. Your phone joins a network named after the post.
3. A setup page opens on your phone by itself. Pick your Wi-Fi network, type its password, and tap **Save**.
4. Once the post is connected, the QR code page closes by itself.

Leave the other fields on the setup page as they are unless you've been told
otherwise. Without Wi-Fi the post keeps measuring and alarming, and sends everything
it saved once it's connected again.

## 6. The Google Sheet

Every reading lands in a shared Google Sheet. Each tab has one job:

| Tab | What's in it | Do you edit it? |
| --- | --- | --- |
| readings | One row per reading, from every post | No |
| posts | One row per post: when it was last heard from, battery, safe range | Only the band columns |
| config | Who gets alert emails, and the safe range for every post | Yes |
| firmware | Software versions for the posts | No |

Readings older than a month move to a separate archive spreadsheet, one per month,
in the same Drive folder.

**Alert emails** go to the addresses in config › `alert_to`. You get one when
something goes wrong, one when it's fixed, and a reminder while it lasts. They cover
too hot or too cold, a low battery, and a post that has stopped reporting.

**To change the safe range** (in °C):

- For **every post**: fill in `band_min_c` and `band_max_c` in the config tab.
- For **one post**: fill in the same two columns in that post's row of the posts tab. This overrides config.
- **Blank** in both places lets each post use the range set on its own screen.

A post picks up the change the next time it sends readings: within a minute if it's
plugged in or its screen is on, or within about 15 minutes if it's on battery with its
screen dark.

## 7. If something looks wrong

| You see | Try this |
| --- | --- |
| The screen is black | Tap it. If nothing happens, plug in the USB-C cable. |
| NO READING, or an amber number | Check the sensor's cable is pushed in at both ends. It recovers by itself once it's reseated. |
| The clock shows "no clock" | It hasn't reached Wi-Fi yet. Check Menu › Network, or join Wi-Fi again (section 5). |
| The Sheet isn't updating | Check the post's Wi-Fi first. A post on battery with its screen dark sends every 15 minutes or so, so give it time. |
| It alarms but the tank is fine | The safe range may be wrong for this tank. Check it on the screen's top right, then change it on the post or in the Sheet. |
| Anything else | Note what the screen shows, take a photo, and send it to whoever gave you the post. |
