// the login and world-entry messages as the real server sends them (from personal-settings-and-backpack-upgrade.pcapng)
#pragma once
#include "game.hpp"

static const char *NPC_XML_INDEXES = "Axebeak|AxebeaksBoss|BladeofRevolt|Chest|CrystalTurret|DiamondDestructoBoss|DragonsBreath|DreadGazerBoss|FlippingSpikesHorizontal|FlippingSpikesHorizontalDown|FlippingSpikesVertical|FlippingSpikesVerticalRight|HealingAura|HiddenObject|IceGolemBoss|Jelatinous|JelatinousChunkBoss|MagicCrystalBallBoss|MimicChest|PlayerShield|PoisonStatusEffect|Portal|Servant Of K'Tesh|Shield|ShieldCrystal|Shriekgazer|Swordsman|TerraShark|Viper|WindGuardian|WindGuardianShield";
// "(code)|glyph|available": the client turns (wl), (lol) ... into emoticons
static const char *EMOTICONS = "(wl)|ā|0&(yes)|Ă|0&(no)|ă|1&(love)|Ą|0&(oops)|ą|0&(shy)|Ć|1&(wink)|ć|1&(tongue)|Ĉ|1&(agree)|ĉ|1&(sleep)|Ċ|0&(punch)|ċ|0&(music)|Č|1&(build)|č|1&(megaphone)|Ď|0&(sigh)|ď|1&(mad)|Đ|1&(wow)|đ|1&(dance)|Ē|1&(see-no-evil)|ē|1&(bheart)|Ĕ|0&(heart)|ĕ|0&(grow)|Ė|0&(gems)|ė|0&(kiss)|Ę|1&(gtoken)|ę|0&(lol)|Ě|1&(smile)|Ā|1&(cool)|Ĝ|0&(cry)|ĝ|0&(vend)|Ğ|0&(bunny)|ě|0&(cactus)|ğ|0&(pine)|Ĥ|1&(peace)|ģ|0&(terror)|ġ|0&(troll)|Ġ|0&(evil)|Ģ|0&(fireworks)|Ħ|0&(football)|ĥ|0&(alien)|ħ|0&(party)|Ĩ|0&(pizza)|ĩ|0&(clap)|Ī|0&(song)|ī|1&(ghost)|Ĭ|0&(nuke)|ĭ|0&(halo)|Į|0&(turkey)|į|0&(gift)|İ|0&(cake)|ı|0&(heartarrow)|Ĳ|0&(lucky)|ĳ|1&(shamrock)|Ĵ|0&(grin)|ĵ|0&(ill)|Ķ|0&(eyes)|ķ|0&(weary)|ĸ|0&(moyai)|ļ|0&(plead)|Ľ|0&(fire)|ľ|0&(flirty)|Ŀ|0&(lightbulb)|ŀ|0&(moneymouth)|Ł|0&(nerd)|ł|0&(skull)|Ń|0&(trophy)|ń|0&(wilted)|Ņ|0&";
// the feature numbers the real server switches on (base64 of: count, numbers)
static const char *FEATURE_FLAGS = "DxADDA0PAgoIDgEJBhEEBQ==";

inline void send_emoticons(Player *p) { call(p->peer, {"OnEmoticonDataChanged", U32{219132872}, EMOTICONS}); }

// after the full login text, before OnSuperMainStart...
inline void send_login_preamble(Player *p)
{
    call(p->peer, {"OnSetNpcXMLIndexes", NPC_XML_INDEXES});
    call(p->peer, {"OnOverrideGDPRFromServer", 100, 1, 0, 1});
    call(p->peer, {"OnSetRoleSkinsAndTitles", "000000", "000000"});
}

// what the world gets sent right after the map: emoticons, the pearl currency hidden, and a bare SPECIAL packet
inline void send_world_preamble(Player *p)
{
    send_emoticons(p);
    call(p->peer, {"HidePearlCurrency"});
    GamePacket h; h.type = 0x23; h.netID = -1; h.tileX = -1; h.tileY = -1; send_packet(p->peer, h);
}

// 1234567 -> "1,234,567"
inline std::string thousands(long n) { std::string s = std::to_string(n), o; int c = 0; for (int i = (int)s.size() - 1; i >= 0; --i) { o.insert(o.begin(), s[i]); if (++c % 3 == 0 && i > 0) o.insert(o.begin(), ','); } return o; }
