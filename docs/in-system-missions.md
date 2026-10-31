# In-system missions

New game content for flying inside one star system: an Earth start in an Interplanetary Shuttle, Passenger
Seats, and bulletin-board **shuttle runs** (passengers to another starport in the same system). All of it is
68k code in `fe2/fe2.s` and `fe2/fe2_modded.s` (hooks of the same size as the code they replace, new code at
the end of the file), so it is in both builds and could be patched into the original executables like the
bug fixes. Job data lives in the game's own memory (the passenger list), so it is saved with the commander.

## 1. Earth start (title key 4)

Start code 3 (unused by the game). Landed at London in an Interplanetary Shuttle (model 10) with Atmospheric
Shielding and Sleeper Berths, 100 credits. Keys 5 and 6 are now "last saved position" and "load from disc".
Hooks `Lstart_keys`, `Lstart_body`, `Lstart_ship`. The shuttle's data was changed: capacity 4 t -> 7 t (the
built-in drive takes 4 t) and reverse/vertical thrust -1635 -> -2500 (it could not lift off from Earth).

## 2. Sleeper Berths

Equipment flag bit 20 of the ship's equipment long (`158(ship)`), 2 t, 100 credits, sold everywhere.
Four places for passengers, on top of cabins (`740(a6)`), for shuttle runs and the game's own passenger
adverts. The cabin figure under the station and ship screens counts them (`Lseats_figures`), and the
game's passenger advert checks them (`Lseats_free`).

## 3. Shuttle runs

**Adverts.** When the game makes a station news entry (`L5fdca`) in a system with two or more starports,
`Lshuttle_news` turns about 1 in 10 into a shuttle advert (`SH_AD_CHANCE` = `$1a00` of 65536). With
Sol's 18 starports that is about two per board. The advert names another starport of the system:

| Destination | Pay (credits) | Time allowed |
|---|---|---|
| same planet or moon (same frame parent) | 60-149 for one seat | two days |
| another body | 200-599 for one seat | two weeks |

Seats: one (3 in 8), two (2 in 8), three (2 in 8), four (1 in 8); pay is `base x (seats + 1) / 2`.
The news entry is message `$99d5` with d1 = destination object slot, d2 = seat-count text, d3 = pay,
d4 = days `<< 8` + (seats - 1) `<< 2`, a2 = the client's name seed (low byte = destination slot).

**Offer and accept.** Clicking the advert (`Lshuttle_click` on the station screen's item dispatch
`l76468`) shows the client and "OK - agreed." / "Hang up.". Accepting (`Lshuttle_accept`) checks the
passenger roster (59) and free places (cabins + seats), books the seats (`744(a6)`), takes the advert off
the board and adds an ordinary passenger entry through the ship screen module's fn 32 (`L7ec22`):

| Entry (16 bytes at `11960(a6)`) | Shuttle run |
|---|---|
| +0 word pay | pay in credits |
| +2 word flags | (seats - 1) `<< 2`, **bit 0 set** (the game never sets it: its low byte is (n-1)<<2 or `$1c`) |
| +4 long system | the current system |
| +8 long deadline | today + 2 or 14 days |
| +12 long name seed | random, **low byte = destination object slot** |

**Delivery.** After a landing the game lets off the first passenger whose system is the current one
(`L7ee5e`). `Lshuttle_match` replaces that compare: an entry with bit 0 only matches when the starport
landed at (`93(678(a6))`, its object slot) is the booked one; then bit 0 is cleared and the game's own code
pays, thanks and frees the places. Late runs expire like any passenger (the deadline check `l7f0cc`,
reputation -2), and a late named passenger leaves at the next landing in the system (`Lpax_late`).

**Text.** Station strings `$99d5..$99e0` (`Lshuttle_strings`, reached from the station module's fn 40,
`Lseats_strings` in the original build, `Lshuttle_modstr` in the modded build). `$99d9` is not a fixed
string: it prints the starport whose slot is in d1 and its parent body, e.g. "London (Earth)".

## 4. Testing

Measured in a test instance (start 4, London):

- adverts on every board in Sol; the text, the client screen and the accept reply are right; the cabin
  figure shows 3 left, 1 used after one seat is booked;
- a planted shuttle passenger for Moscow did not leave on a landing event at London, and did leave at
  Moscow (the landed-at station pointer `678(a6)` poked to Moscow's object, event 5) with the pay and the
  seat given back.

Not done yet: the ship screen's passenger list shows the system, not the starport; selling the seats (or
a cabin) while they are in use is not refused; a real flight from one starport to another.
