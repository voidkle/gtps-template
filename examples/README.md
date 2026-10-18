# gtbot: scripting bots with Lua

`gtbot` is a Growtopia client for this server that you drive from a Lua script. It logs in through the real flow, so a bot is
a normal player for the server. Build and run:

```sh
make tools/gtbot.out
tools/gtbot.out examples/break_down.lua ../items.dat      # items.dat is optional but needed for item() and for tracking placed blocks
```

Calls that wait (`connect`, `join`, `walk`, `break_tile`, `wait`, `sleep`) keep **all** bots serviced, so one script can run
several bots, and event handlers fire while you wait.

## API

```lua
local bot = Bot.new{ name = "Testkle", password = "...", host = "127.0.0.1", https = 443, port = 17091 }
```

| Call | What it does |
|---|---|
| `bot:connect()` | log in; `true`, or `nil, reason` |
| `bot:join("PLAY")` | enter a world and read its map; `true`, or `nil, reason` ("That world is inaccessible." ...) |
| `bot:leave()` / `bot:disconnect()` | back to the world menu / log out |
| `bot:say("text")` | chat; `/commands` work too |
| `bot:walk(tx, ty [, ms])` | walk to a tile in steps (about 24 px every 90 ms); `bot:teleport(tx, ty)` jumps |
| `bot:position()` | `tx, ty` |
| `bot:punch(tx, ty)` / `bot:place(tx, ty, id)` / `bot:wrench(tx, ty)` | one action on a tile |
| `bot:break_tile(tx, ty [, interval_ms=600 [, max=60]])` | punch until foreground and background are both gone; `true` when clear |
| `bot:tile(tx, ty)` | `fg, bg` as the bot sees them (kept up to date from the server's broadcasts) |
| `bot:world()` | `name, width, height, partial` (`partial` = a tile extra the bot can't read cut the map short) |
| `bot:players()` | list of `{netid, name, x, y}` (tiles) |
| `bot:inventory()`, `bot:count(id)`, `bot:gems()` | what the bot carries |
| `bot:send(type, text)` | raw message, e.g. `bot:send(2, "action|dialog_return\ndialog_name|vending\n...")` |
| `bot:packet{type=3, item=18, tile_x=, tile_y=, x=, y=, flags=}` | raw game packet (defaults: the bot's own position and netID) |
| `bot:on(event, fn)` | `fn(bot, ...)` for `console(msg)`, `talk(msg, netid)`, `dialog(text)`, `spawn(name, netid)`, `remove(name, netid)`, `tile(x, y, fg, bg, by_netid)`, `inventory()`, `login()`, `disconnect()`, `variant(name, args...)` |
| `bot:wait(ms)` / `sleep(ms)` | let time pass |
| `item("World Lock")` / `item(242)` | item id by name / name by id |
| `clock()` | seconds, for timing |

Tile coordinates are tiles, not pixels. The server limits reach to 2 tiles around the player, so walk close before punching.
Not done yet: picking up drops, wearing clothes, reading dialogs into forms; use `bot:send` for those.
