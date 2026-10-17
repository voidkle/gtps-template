# gtps

A Growtopia private server written in C++20 from the protocol documentation, not from another server's code.
It speaks the same login the real game uses, runs worlds, locks, trading, a store, vending machines and more, and
comes with a decoder for packet captures of the real game.

📖 **The protocol guide this server is built from: https://voidkle.github.io/gtps-guide/**
Every packet, action and server function, the login flow, the world format, and what the official server was seen
doing. If you want to know *why* something here is done the way it is, look there.

> Not affiliated with or endorsed by Ubisoft or Growtopia. No game files are included: you bring your own `items.dat`.
> Meant for private servers you run for yourself and your friends.

## What works

| Area | |
|---|---|
| Login | `server_data.php`, a login website (also registers new GrowIDs), saved-token check, the `ltoken` → `OnSendToServer` → full-login handshake, `items.dat` hash check and download |
| Worlds | generated and saved to disk, join/leave, other players, movement, chat bubbles, weather, nuke, world bans |
| Building | break (with real damage/reach rules), place, backgrounds, seeds that grow, splice and harvest, doors with destinations, signs, drops and pickup, every item with a tile extra can be placed |
| Locks | World Lock and its variants, Small/Big/Huge/Builder's Lock with owners, access lists and a public flag, the World Key trade |
| Items | inventory, wear/unwear with mods (double jump, speed, reach, damage), drop/trash, Diamond Lock ↔ 100 World Locks |
| Economy | store from `resources/store.txt`, trading (4 slots, confirm for World Keys), Vending Machines (per item or per lock, warm-up, visitors buy) |
| Social | friends, `/msg`, Social Portal, trade history, notebook, wardrobe presets, trade billboard, My Worlds with home and favorites |
| Levels | XP and levels, daily quest on the phone, Seed Diary lines |
| Moderation | ghost, noclip, nuke, ban, kick, summon, broadcast |

Not built: guilds, mail, auctions, fishing, pets, PvE, and what most tile extras *do* (storage, mannequins, weather
machines…) beyond placing them. The guide documents all of those packets if you want to add them.

## Build

```sh
sudo apt install g++ libssl-dev zlib1g-dev make        # Debian/Ubuntu/Mint
make                                                   # builds gtps.out; ENet is vendored in third_party/enet
```

Linux and macOS (POSIX sockets). The prebuilt `libenet.a` in `third_party/enet/lib` is for x86-64 Linux; on macOS
use `libenet_macos.a`.

## Set up

1. **`items.dat`** — copy yours (version 1 to 27, from the game's cache folder) next to `gtps.out`.
2. **Certificate** — the game only talks to the login website over HTTPS:
   ```sh
   scripts/make-cert.sh        # writes resources/ctx/server.crt and server.key
   ```
   Install `server.crt` as a **trusted root** on every device that logs in (Windows: `certutil -addstore -f Root server.crt`
   in an administrator prompt). Recent Android builds only accept real certificates, so use Windows.
3. **Point the game at your server** — add to the hosts file of the PC that plays
   (`C:\Windows\System32\drivers\etc\hosts`), with your server's address:
   ```text
   192.168.1.10 growtopia1.com
   192.168.1.10 www.growtopia1.com
   192.168.1.10 growtopia2.com
   192.168.1.10 www.growtopia2.com
   ```
4. **Config** — `cp gtps.conf.example gtps.conf` and set `ip` to your server's address.
5. **Port 443** — either run as root, or `sudo sysctl -w net.ipv4.ip_unprivileged_port_start=443`
   (put `net.ipv4.ip_unprivileged_port_start=443` in `/etc/sysctl.d/99-gtps.conf` to keep it).
6. `./gtps.out` — then start Growtopia, and in the login page type a GrowID and password, tick "This is a new GrowID" the first time.

The **first account that registers is the administrator** (`/give`, `/giveto`, `/gems`, `/weather` and the moderator commands).

## Commands

| Who | Commands |
|---|---|
| everyone | `/help` `/who` `/msg <name> <text>` `/warp <world>` `/me <text>` `/trade <name>` `/fav` `/sethome` `/home` |
| world owner | `/pull` `/kick` `/worldban <name>`, `/worldunban <name>`, `/uba` (or the buttons when you wrench a player) |
| moderator | `/ghost` `/noclip` `/nuke [world]` `/unnuke <world>` `/ban <name> <hours|perm> [reason]` `/unban` `/disconnect` `/summon` `/warpto` `/broadcast` |
| administrator | `/give <id> [n]` `/giveto <name> <id> [n]` `/gems <n>` `/weather <id>` |

**Operator console:** while the server runs, lines you append to `data/commands.txt` are executed once and the file
is removed: `giveto <name> <id> [n]`, `gems <name> <n>`, `passwd <name> <new password>`.

## Data and tuning

| Path | |
|---|---|
| `data/accounts/*.acc` | accounts (passwords are PBKDF2-hashed), inventory, friends, notes… plain text |
| `data/worlds/*.wld` | worlds |
| `resources/mods.txt` | worn-item effects and their messages |
| `resources/punch.txt` | hand-item punch patterns (Rayman's line of 3) and reach |
| `resources/store.txt` | the store |
| `resources/dialogs/`, `resources/achievements.txt` | static dialogs and the achievement list shown in the profile |

## Tests and tools

```sh
make test/e2e.out tools/gtcap.out test/bot.out test/extras_test.out
# start a server with a test config (https_port 9447, port 17097, a throwaway data_dir), then:
cd test && ./e2e.out 9447 17097 ../items.dat        # two scripted clients: login, build, locks, trade, vending…
./extras_test.out                                    # every tile extra against the guide's layouts
```

- **`tools/gtcap`** decodes a Wireshark/tcpdump capture of the real game's traffic into readable packets, so you can
  compare what the official server does with yours (see the guide's *Capturing the real game*).
- **`test/bot.cpp`** is a small player that logs in and stands in a world; it accepts friend and trade requests.
- **`tools/udpcap.py`** records this repo's own traffic into a pcap.

## Layout

```text
src/proto.hpp     wire helpers: buffers, game packets, variant lists
src/items.hpp     items.dat reader (versions 1 to 27)
src/world.hpp     world model, generator, save format
src/extras.hpp    tile extras (the empty extra for every item type)
src/store.hpp     accounts and login tokens
src/https.hpp     the TLS login website
src/main.cpp      game logic        src/features.hpp  profile, social, vending, quests
```

## Contributing

Issues and pull requests are welcome. When something differs from the real game, a capture decoded with `gtcap` (names
removed) is the most useful bug report.

## License

MIT, see [LICENSE](LICENSE). Third-party parts are listed in [NOTICE](NOTICE). The guide at
[voidkle.github.io/gtps-guide](https://voidkle.github.io/gtps-guide/) has its own license.
