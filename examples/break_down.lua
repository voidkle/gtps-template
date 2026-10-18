-- walk 3 blocks to the right of the spawn point, then break the 3 tiles below (foreground and background), one punch every 600 ms
local bot = Bot.new{ name = "Testkle", password = "your-password", host = "127.0.0.1" }
assert(bot:connect())
assert(bot:join("PLAY"))
local x, y = bot:position()
bot:walk(x + 3, y)
bot:walk(x + 3, y + 1)                 -- one block down, so the third tile is within reach
for dy = 1, 3 do
  local ok = bot:break_tile(x + 3, y + dy, 600)
  local fg, bg = bot:tile(x + 3, y + dy)
  print(("tile %d,%d broken=%s fg=%d bg=%d"):format(x + 3, y + dy, tostring(ok), fg, bg))
end
bot:disconnect()
