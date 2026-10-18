// features decoded from captures of the official server (docs/guide/observed.md). Included by main.cpp after the helpers it uses.
#pragma once

// ---------------------------------------------------------------- data files
static std::string read_text(const std::string &path) { std::ifstream f(path); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static std::string res_dialog(const std::string &name) { return read_text(S.cfg.res_dir + "/dialogs/" + name + ".txt"); }
static std::vector<std::string> g_achievements;
static void load_achievements()
{
    std::istringstream in(read_text(S.cfg.res_dir + "/achievements.txt"));
    for (std::string l; std::getline(in, l); ) if (!l.empty()) g_achievements.push_back(l);
}
static void dialog_later(Player *p, const std::string &text, int ms) { call(p->peer, {"OnDialogRequest", text}, -1, ms); }
static void bubble_all(World &w, Player *p, const std::string &text, u32 a = 0, u32 b = 0)
{
    broadcast(w.name, [&](Player *q) { call(q->peer, {"OnTalkBubble", U32{(u32)p->netid}, text, U32{a}, U32{b}}); });
}

// ---------------------------------------------------------------- profile (wrench yourself)
static std::string effects_lines(Account &a)
{
    std::string s;
    for (u16 c : a.clothes) { Mod m; if (c && mod_for(c, m) && !m.name.empty()) s += "add_label_with_icon|small|`w" + m.name + "``|left|" + std::to_string(c) + "|\n"; }
    return s;
}

static void profile_self(Player *p)
{
    Account &a = *p->acct; World *w = S.world_of(p); std::string nid = std::to_string(p->netid);
    int people = 0; for (Player *q : S.in_world(p->world)) if (!q->ghost || q == p) ++people;
    std::string s = "embed_data|netID|" + nid + "\nadd_popup_name|WrenchMenu|\nset_default_color|`o\nadd_player_info|" + shown_name(w, p) + "|" + std::to_string(a.level) + "|" + std::to_string(a.xp) + "|" + std::to_string(50 * (a.level * a.level + 2)) + "|\n"
        "add_spacer|small|\nadd_spacer|small|\nadd_player_info_small|`9Card Battle Level``|1|0|6300|\nadd_spacer|small|\nset_custom_spacing|x:5;y:10|\n";
    for (auto [nm, img] : std::vector<std::pair<const char *, const char *>>{{"set_online_status", "online_status_1green"}, {"billboard_edit", "edit_billboard"}, {"wardrobe_customization", "wardrobe"},
         {"seed_diary_customization", "seed_diary"}, {"notebook_edit", "notebook"}, {"guildnotebook_show", "guild_notebook"}, {"goals", "goals_quests"}, {"bonus", "daily_bonus_active"},
         {"my_worlds", "my_worlds"}, {"alist", "achievements"}})
        s += std::string("add_custom_button|") + nm + "|image:interface/large/gui_wrench_" + img + ".rttex;image_size:400,260;width:0.19;|\n";
    s += "add_custom_label|(0/" + std::to_string(g_achievements.size()) + ")|target:alist;top:0.72;left:0.5;size:small|\n";
    for (auto [nm, img] : std::vector<std::pair<const char *, const char *>>{{"emojis", "growmojis"}, {"marvelous_missions", "marvelous_missions"}, {"title_edit", "title"}, {"trade_scan", "trades"}})
        s += std::string("add_custom_button|") + nm + "|image:interface/large/gui_wrench_" + img + ".rttex;image_size:400,260;width:0.19;|\n";
    s += "embed_data|netID|" + nid + "\n";
    for (auto [nm, img] : std::vector<std::pair<const char *, const char *>>{{"pets", "battle_pets"}, {"wrench_customization", "customization"}, {"open_worldlock_storage", "auction"}})
        s += std::string("add_custom_button|") + nm + "|image:interface/large/gui_wrench_" + img + ".rttex;image_size:400,260;width:0.19;|\n";
    s += "add_custom_break|\nadd_spacer|small|\nset_custom_spacing|x:0;y:0|\nadd_spacer|small|\nadd_textbox|Surgeon Level: 0|left|\nadd_spacer|small|\nadd_textbox|`wActive effects:``|left|\n" + effects_lines(a) + "add_spacer|small|\n"
         "add_smalltext|Fires Put Out: 0|left|\nadd_spacer|small|\nadd_textbox|`oYou have `w" + std::to_string(a.backpack) + "`` backpack slots.``|left|\n"
         "add_textbox|`oCurrent world: `w" + p->world + "`` (`w" + std::to_string((int)(p->x / 32)) + "``, `w" + std::to_string((int)(p->y / 32)) + "``) (`w" + std::to_string(people) + "`` person)````|left|\n"
         "add_smalltext|`oHome World:" + (a.home.empty() ? std::string("none") : a.home) + "``|left|\nadd_spacer|small|\n"
         "add_textbox|`oTotal time played is `w" + hours_played(p) + "`` hours.  This account was created `w" + age_days(a) + "`` days ago.``|left|\nadd_spacer|small|\nend_dialog|popup||Continue|\nadd_quick_exit|\n";
    dialog(p, s);
}

// ---------------------------------------------------------------- online status, billboard
static void status_dialog(Player *p)
{
    int st = p->acct->status;
    dialog(p, "set_default_color|`o\nadd_label_with_icon|big| `wOnline Status`` |left|1366|\nadd_spacer|small|\nadd_checkbox|checkbox_status_online|Online|" + std::string(st == 0 ? "1" : "0") +
              "\nadd_checkbox|checkbox_status_busy|Busy|" + (st == 1 ? "1" : "0") + "\nadd_checkbox|checkbox_status_away|Away|" + (st == 2 ? "1" : "0") + "\nadd_button||Ok|noflags|0|0|\nend_dialog|set_online_status||\nadd_quick_exit|\n");
}

static void send_billboard(Player *to, Player *of)
{
    Account &a = *of->acct; if (!a.bb_item) return;
    call(to->peer, {"OnBillboardChange", U32{(u32)of->netid}, a.bb_item, std::to_string(a.bb_show) + "," + std::to_string(a.bb_buying), a.bb_price, a.bb_peritem}, of->netid);
}

static void billboard_dialog(Player *p, int pending_item = -1)
{
    Account &a = *p->acct; int item = pending_item >= 0 ? pending_item : a.bb_item;
    std::string s = "set_default_color|`o\nadd_label_with_icon|big|`wTrade Billboard``|left|8282|\nadd_spacer|small|\n";
    if (item > 0 && S.items.get(item) && inv_count(a, item) > 0) s += "add_label_with_icon|small|`w" + S.items.get(item)->name + "``|left|" + std::to_string(item) + "|\n";
    else s += "add_textbox|`4Selected item doesn't exist in your inventory.``|left|\n";
    s += "add_item_picker|billboard_item|`wSelect Billboard Item``|Choose an item to put on your billboard!|\nadd_spacer|small|\nadd_checkbox|billboard_toggle|`$Show Billboard``|" + std::string(a.bb_show ? "1" : "0") +
         "\nadd_checkbox|billboard_buying_toggle|`$Is Buying``|" + (a.bb_buying ? "1" : "0") + "\nadd_text_input|setprice|Price of item:|" + std::to_string(a.bb_price) + "|5|\n"
         "add_checkbox|chk_peritem|World Locks per Item|" + (a.bb_peritem ? "1" : "0") + "\nadd_checkbox|chk_perlock|Items per World Lock|" + (a.bb_peritem ? "0" : "1") + "\nadd_spacer|small|\nend_dialog|billboard_edit|Close|Update|\n";
    dialog(p, s);
}

// ---------------------------------------------------------------- notebook (5 pages)
static void notebook_view(Player *p, int page)
{
    page = std::clamp(page, 0, 4);
    std::string s = "set_default_color|`o\nstart_custom_tabs|\n";
    for (int i = 0; i < 5; ++i) s += "add_custom_button|viewPnPage_" + std::to_string(i) + "|image:interface/large/btn_tabs1.rttex;image_size:228,92;frame:" + (i == page ? "1" : "0") + "," + std::to_string(i) + ";width:0.15;|\n";
    const std::string &t = p->acct->notes[page];
    s += "end_custom_tabs|\nadd_label|big|Notebook|left|0|\nadd_spacer|small|\nadd_textbox|" + (t.empty() ? std::string("`4No saved note to display.``") : t) + "|left|\nadd_spacer|small|\n"
         "add_button|editPnPage|Edit|noflags|0|0|\nadd_button|back|Back|noflags|0|0|\nembed_data|pageNum|" + std::to_string(page) + "\nend_dialog|paginated_personal_notebook_view|||\n";
    dialog_later(p, s, 500);
}
static void notebook_edit(Player *p, int page)
{
    dialog(p, "set_default_color|`o\nadd_label|big|Notebook|left|0|\nadd_spacer|small|\nadd_textbox|Page " + std::to_string(page + 1) + "|left|\nadd_spacer|small|\nadd_text_box_input|personal_note||" + p->acct->notes[page] + "|256|7|\n"
              "add_smalltext|* Max `5256`` characters!|left|\nadd_smalltext|* Type `5<`5CR``>`` to add a new line.|left|\nadd_spacer|small|\nadd_button|save|Save|noflags|0|0|\nadd_button|clear|Clear|noflags|0|0|\n"
              "add_button|back|Cancel|noflags|0|0|\nembed_data|pageNum|" + std::to_string(page) + "\nend_dialog|paginated_personal_notebook_edit||\nadd_quick_exit|\n");
}

// ---------------------------------------------------------------- my worlds
static void worlds_tab(Player *p, int tab)
{
    Account &a = *p->acct;
    std::string s = "set_default_color|`o\nstart_custom_tabs|\n";
    for (int i = 0; i < 3; ++i) s += "add_custom_button|myWorldsUiTab_" + std::to_string(i) + "|image:interface/large/btn_tabs2.rttex;image_size:228,92;frame:" + (i == tab ? "1" : "0") + "," + std::to_string(i) + ";width:0.15;|\n";
    s += "end_custom_tabs|\n";
    auto world_button = [&](const std::string &n) { s += "add_button|" + n + "|" + n + "|noflags|0|0|\n"; };
    if (tab == 0)
    {
        s += "add_label|big|Locked Worlds|left|0|\nadd_spacer|small|\nadd_textbox|Place a World Lock in a world to lock it. Break your World Lock to unlock a world.|left|\nadd_spacer|small|\n";
        for (auto &n : a.my_worlds) world_button(n);
    }
    else if (tab == 1)
    {
        s += "add_label|big|Home World|left|0|\nadd_spacer|small|\nadd_textbox|Use /sethome to assign a world as your home world.|left|\nadd_textbox|Use /home to warp directly to your home world.|left|\nadd_spacer|small|\n";
        if (!a.home.empty()) world_button(a.home);
    }
    else
    {
        s += "add_label|big|Favorited Worlds|left|0|\nadd_spacer|small|\nadd_textbox|Use /fav in a world to add or remove a favorite world.|left|\nadd_spacer|small|\nadd_textbox|Favorite Worlds: " + std::to_string(a.favs.size()) + "/10|left|\nadd_spacer|small|\n";
        for (auto &n : a.favs) world_button(n);
    }
    dialog_later(p, s + "add_spacer|small|\nend_dialog|worlds_list||Back|\nadd_quick_exit|\n", 500);
}

// ---------------------------------------------------------------- achievements, titles, storage, social pages
static void achievements_dialog(Player *p)
{
    std::string s = "set_default_color|`o\nadd_label_with_icon|small|" + p->name() + "'s Achievements|left|982|\nadd_spacer|small|\n";
    for (auto &t : g_achievements) s += "add_achieve|" + t + "|Not achieved!|left|127|\n";
    dialog(p, s + "add_spacer|small|\nend_dialog|alist||Back|\nadd_quick_exit|\n");
}

static void trade_history_dialog(Player *p)
{
    std::string s = "set_default_color|`o\nadd_label_with_icon|small|" + p->name() + "'s Trade History|left|242|\nadd_spacer|small|\n";
    auto &t = p->acct->trades;
    if (t.empty()) s += "add_smalltext|`oNo trades yet.``|left|\n";
    for (int i = (int)t.size() - 1, n = 0; i >= 0 && n < 10; --i, ++n) s += "add_smalltext|" + t[i] + "|left|\nadd_spacer|small|\n";
    dialog(p, s + "add_button|socialportal|Back|noflags|0|0|\nadd_button||Close|noflags|0|0|\nadd_quick_exit|\nend_dialog|playerportal|||\n");
}

static std::string clock_text() { time_t tt = time(nullptr); tm lt; localtime_r(&tt, &lt); char b[32]; strftime(b, sizeof b, "%H:%M on %m/%d.", &lt); return b; }
static std::string offer_text(const std::map<int, int> &o)
{
    if (o.empty()) return "Nothing";
    std::string s; for (auto &[id, n] : o) { if (!s.empty()) s += ", "; s += std::to_string(n) + " " + (S.items.get(id) ? S.items.get(id)->name : "?"); } return s;
}
static void record_trade(Player *a, Player *b, const std::map<int, int> &gave, const std::map<int, int> &got)
{
    auto line = [&](Player *me, Player *other, const std::map<int, int> &g, const std::map<int, int> &r) {
        me->acct->trades.push_back("`9You traded [" + offer_text(g) + "] with `1" + other->name() + "`` in `2" + me->world + "`` and received [" + offer_text(r) + "] `0at " + clock_text() + "``");
        if (me->acct->trades.size() > 30) me->acct->trades.erase(me->acct->trades.begin());
    };
    line(a, b, gave, got); line(b, a, got, gave);
}

static void friends_page(Player *p, bool all)
{
    Account &a = *p->acct; int online = 0;
    for (i32 uid : a.friends) if (online_user(uid)) ++online;
    std::string s = "set_default_color|`o\nadd_label_with_icon|big|" + std::to_string(online) + " of " + std::to_string(a.friends.size()) + " `wFriends Online``|left|1366|\nadd_spacer|small|\n";
    if (all) s += "add_button|close|`wClose``|noflags|0|0|\n";
    else if (!online) s += "add_textbox|`oNone of your friends are currently online.``|left|\nadd_spacer|small|\nadd_spacer|small|\n";
    for (i32 uid : a.friends)
    {
        std::string nm; { std::lock_guard g(S.store.mu); nm = S.store.name_of(uid); Account *fa = S.store.find(nm); (void)fa; }
        Player *q = online_user(uid);
        if (q) s += "add_friend_image_label_button|" + std::to_string(uid) + "|`2" + nm + "``|game/tiles_page14.rttex|1.6|31|23|32|false|false|\n";
        else if (all)
        {
            i64 seen = 0; { std::lock_guard g(S.store.mu); if (Account *fa = S.store.find(nm)) seen = fa->last_seen; }
            s += "add_friend_image_label_button|" + std::to_string(uid) + "|`4(" + std::to_string(seen ? (now_s() - seen) / 86400 : 0) + "d) ``" + nm + "|game/tiles_page14.rttex|1.6|31|23|32|false|false|\n";
        }
    }
    if (!all) s += "add_button|friend_all|Show offline and ignored too|noflags|0|0|\nadd_button|all_friends|Edit Friends|noflags|0|0|\nadd_button|friends_options|Friend Options|noflags|0|0|\nadd_button|back|Back|noflags|0|0|\n";
    dialog(p, s + "add_button||Close|noflags|0|0|\nend_dialog|friends|||\nadd_quick_exit|\n");
}

static void social_portal(Player *p) { dialog(p, res_dialog("social_portal")); }

// ---------------------------------------------------------------- wardrobe
static void send_presets(Player *p)
{
    Account &a = *p->acct; std::string j = "{\"PresetCount\":" + std::to_string(a.preset_count) + ",\"Presets\":[";
    for (int i = 0; i < 4; ++i) { if (i) j += ","; j += "\""; for (int k = 0; k < 10; ++k) { if (k) j += ","; j += std::to_string(a.presets[i][k]); } j += "\""; }
    call(p->peer, {"OnWardrobePresetUpdate", j + "]}"});
}
static void wardrobe_action(Player *p, const std::string &action, const std::string &text)
{
    Account &a = *p->acct; int n = atoi(field(text, "preset").c_str()) - 1;
    auto resend = [&] { if (!p->world.empty()) broadcast(p->world, [&](Player *q) { send_clothing(q, p); }); else send_clothing(p, p); };
    if (action == "wardrobe_preset_load") send_presets(p);
    else if (action == "wardrobe_load_ui" && n >= 0 && n < a.preset_count)
    {
        for (int i = 0; i < 10; ++i)
        {
            u16 id = a.presets[n][i]; if (id && inv_count(a, id) < 1) id = a.clothes[i];
            if (id != a.clothes[i] && id) call(p->peer, {"OnEquipNewItem", (i32)id}, p->netid);
            a.clothes[i] = id;
        }
        sfx(p, "audio/dialog_confirm.wav"); resend();
    }
    else if (action == "wardrobe_save_ui" && n >= 0 && n < a.preset_count) { for (int i = 0; i < 10; ++i) a.presets[n][i] = a.clothes[i]; send_presets(p); sfx(p, "audio/dialog_confirm.wav"); }
    else if (action == "wardrobe_unequip_all") { for (u16 &c : a.clothes) c = 0; resend(); }
    else if (action == "wardrobe_purchase_slot") console(p, "`oMore wardrobe slots aren't for sale on this server.``");
}

// ---------------------------------------------------------------- seed diary lines
static void diary_plant(Player *p, int seed, bool by_splice)
{
    const Item *it = S.items.get(seed); if (!it || p->world.empty()) return;
    std::string nm = it->name; if (nm.size() > 5 && nm.compare(nm.size() - 5, 5, " Seed") == 0) nm.resize(nm.size() - 5);
    World *w = S.world_of(p); if (!w) return;
    auto say = [&](const std::string &t) { call(p->peer, {"OnTalkBubble", U32{(u32)p->netid}, t, U32{0}, U32{1}}); console(p, t); };
    if (p->acct->seen_planted.insert(seed).second) say("New Item Planted: `2" + nm + "`` has been logged in your Seed Diary");
    if (by_splice && p->acct->seen_found.insert(seed).second) say("New Item Discovered: `2" + nm + "`` has been logged in your Seed Diary");
}

static void diary_found_seed(Player *p, int seed)
{
    const Item *it = S.items.get(seed); if (!it || it->type != A_SEED || !p->acct->seen_found.insert(seed).second) return;
    std::string nm = it->name; if (nm.size() > 5 && nm.compare(nm.size() - 5, 5, " Seed") == 0) nm.resize(nm.size() - 5);
    std::string t = "New Item Discovered: `2" + nm + "`` has been logged in your Seed Diary";
    call(p->peer, {"OnTalkBubble", U32{(u32)p->netid}, t, U32{0}, U32{1}}); console(p, t);
}

// ---------------------------------------------------------------- World Locks as money: Diamond Locks count as 100, and are broken for change
static void send_add(Player *p, int id, int n) { GamePacket g; g.type = 0x0d; g.itemID = id; g.buildRange = std::min(n, 255); send_packet(p->peer, g); }
static int wl_total(Account &a) { return inv_count(a, 242) + 100 * inv_count(a, 1796); }
static bool pay_wl(Player *p, int cost)
{
    Account &a = *p->acct; if (wl_total(a) < cost) return false;
    int wl = inv_count(a, 242), take = std::min(wl, cost); cost -= take;
    if (take) { inv_remove(a, 242, take); send_remove(p, 242, take); }
    if (cost > 0)   // not enough single World Locks: break Diamond Locks and give the change back
    {
        int k = (cost + 99) / 100, change = k * 100 - cost;
        inv_remove(a, 1796, k); send_remove(p, 1796, k);
        if (change) { int give = std::min(change, 200 - inv_count(a, 242)); if (give > 0) { inv_add(a, 242, give); send_add(p, 242, give); } }
    }
    return true;
}
static void give_wl(Player *p, int n) // earnings come back as Diamond Locks where they fill a stack
{
    Account &a = *p->acct; int dl = n / 100, wl = n % 100;
    if (wl) { inv_add(a, 242, wl); send_add(p, 242, wl); }
    if (dl) { inv_add(a, 1796, dl); send_add(p, 1796, dl); }
}

// ---------------------------------------------------------------- vending machine
struct Vend { int item = 0, stock = 0, money = 0; i64 ready = 0; int owner = 0; };
static Vend vend_of(const Tile &t)
{
    Vend v; if (t.extra.size() >= 5) memcpy(&v.item, t.extra.data() + 1, 4);
    long rd = 0; int item = -1; sscanf(t.aux.c_str(), "%d %d %ld %d %d", &v.stock, &v.money, &rd, &v.owner, &item); v.ready = rd;
    if (item >= 0) v.item = item;   // the tile itself shows item 0 while the machine is out of order
    return v;
}
static int vend_price(const Tile &t) { i32 p = 0; if (t.extra.size() >= 9) memcpy(&p, t.extra.data() + 5, 4); return p; }
// per lock (negative price) the machine sells whole packs of -price: with fewer in stock than one pack it is out of order
static bool vend_usable(const Vend &v, int price) { return v.item && v.stock > 0 && price != 0 && (price > 0 || v.stock >= -price); }
static void vend_store(World &w, Tile &t, int x, int y, const Vend &v, int price)
{
    Buf b; b.put<u8>(0x18).put<u32>(vend_usable(v, price) ? v.item : 0).put<i32>(price); t.extra = b.d; t.flags |= T_EXTRA;
    if (v.stock > 0) t.flags |= T_ON; else t.flags &= ~T_ON;
    t.aux = std::to_string(v.stock) + " " + std::to_string(v.money) + " " + std::to_string(v.ready) + " " + std::to_string(v.owner) + " " + std::to_string(v.item);
    w.dirty = true; sync_tile(w, x, y);
}
static const char *UPGRADE = "add_smalltext|Upgrade to a DigiVend Machine for `44,000 Gems``.|left|\nadd_button|upgradedigital|Upgrade to DigiVend|noflags|0|0|\n";

static void vend_owner_dialog(Player *p, World &w, int x, int y)
{
    Tile &t = *w.at(x, y); Vend v = vend_of(t); int price = vend_price(t);
    std::string s = "set_default_color|`o\nadd_label_with_icon|big|`wVending Machine``|left|2978|\nadd_spacer|small|\nembed_data|tilex|" + std::to_string(x) + "\nembed_data|tiley|" + std::to_string(y) + "\n";
    if (!v.item || v.stock <= 0)
        s += "add_textbox|This machine is empty.|left|\nadd_item_picker|stockitem|`wPut an item in``|Choose an item to put in the machine!|\n" + std::string(UPGRADE) + "add_spacer|small|\nend_dialog|vending|Close||\n";
    else
    {
        const Item *it = S.items.get(v.item);
        s += "add_label_with_icon|sml|The machine contains a total of " + std::to_string(v.stock) + " `2" + it->name + "``. |left|" + std::to_string(v.item) + "|\nadd_spacer|small|\n";
        if (!price) s += "add_textbox|Not currently for sale!|left|\n";
        else if (price < 0 && v.stock < -price) s += "add_textbox|`4Out of order: it needs at least " + std::to_string(-price) + " " + it->name + " in it to sell a pack.``|left|\n";
        else if (price > 0) s += "add_textbox|For a cost of:|left|\nadd_label_with_icon|small|" + std::to_string(price) + " x `8World Locks``|left|242|\nadd_spacer|small|\nadd_textbox|You will get:|left|\nadd_label_with_icon|small|1 x `2" + it->name + "``|left|" + std::to_string(v.item) + "|\nadd_spacer|small|\n";
        else s += "add_textbox|For a cost of:|left|\nadd_label_with_icon|small|1 x `8World Lock``|left|242|\nadd_spacer|small|\nadd_textbox|You will get:|left|\nadd_label_with_icon|small|" + std::to_string(-price) + " x `2" + it->name + "``|left|" + std::to_string(v.item) + "|\nadd_spacer|small|\n";
        if (v.money > 0) s += "add_button|pullmoney|Collect `2" + std::to_string(v.money) + " World Locks``|noflags|0|0|\n";
        s += "add_button|pullstock|Empty the machine|noflags|0|0|\nadd_smalltext|`5(Vending Machine will not function when price is set to 0)``|left|\nadd_text_input|setprice|Price|" + std::to_string(std::abs(price)) + "|5|\n"
             "add_checkbox|chk_peritem|World Locks per Item|" + (price >= 0 ? "1" : "0") + "\nadd_checkbox|chk_perlock|Items per World Lock|" + (price < 0 ? "1" : "0") + "\n" + UPGRADE + "add_spacer|small|\nend_dialog|vending|Close|Update|\n";
    }
    dialog(p, s);
}

static void vend_buy_dialog(Player *p, World &w, int x, int y)
{
    Tile &t = *w.at(x, y); Vend v = vend_of(t); int price = vend_price(t);
    if (!vend_usable(v, price)) { bubble(p, p, "`wOUT OF ORDER``"); return; }
    if (v.ready > now_s()) { call(p->peer, {"OnTalkBubble", U32{(u32)p->netid}, "`9WARMING UP!``\nReady in : " + std::to_string(v.ready - now_s()) + " sec", U32{0}, U32{0}}); return; }
    const Item *it = S.items.get(v.item);
    std::string cost = price > 0 ? std::to_string(price) + " x `8World Locks``" : "1 x `8World Lock``", get = price > 0 ? "1" : std::to_string(-price);
    dialog(p, "set_default_color|`o\nadd_label_with_icon|big|`wVending Machine``|left|2978|\nadd_spacer|small|\nembed_data|tilex|" + std::to_string(x) + "\nembed_data|tiley|" + std::to_string(y) + "\n"
              "add_label_with_icon|sml|The machine contains a total of " + std::to_string(v.stock) + " `2" + it->name + "``. |left|" + std::to_string(v.item) + "|\nadd_spacer|small|\nadd_textbox|For a cost of:|left|\n"
              "add_label_with_icon|small|" + cost + "|left|242|\nadd_spacer|small|\nadd_textbox|You will get:|left|\nadd_label_with_icon|small|" + get + " x `2" + it->name + "``|left|" + std::to_string(v.item) + "|\nadd_spacer|small|\n"
              "add_textbox|You have " + std::to_string(wl_total(*p->acct)) + " World Locks.|left|\nadd_text_input|buycount|How many would you like to buy?|0|3|\nembed_data|expectprice|" + std::to_string(price) + "\nembed_data|expectitem|" + std::to_string(v.item) + "\nend_dialog|vending|Close|Buy|\n");
}

static void vend_wrench(Player *p, World &w, int x, int y)
{
    Vend v = vend_of(*w.at(x, y));
    if (v.owner == p->acct->user_id || v.owner == 0) vend_owner_dialog(p, w, x, y); else vend_buy_dialog(p, w, x, y);
}

static void vend_dialog_return(Player *p, World &w, const std::string &name, const std::string &text)
{
    int x = atoi(field(text, "tilex").c_str()), y = atoi(field(text, "tiley").c_str()); Tile *t = w.at(x, y);
    if (!t || !S.items.get(t->fg) || S.items.get(t->fg)->type != 62) return;
    Account &a = *p->acct; Vend v = vend_of(*t); int price = vend_price(*t);
    auto line = [&](const std::string &msg) { bubble_all(w, p, msg); broadcast(w.name, [&](Player *q) { console(q, msg); }); };
    if (!field(text, "expectprice").empty()) LOG("[vending] %s: buycount=%s verify=%s expect=%s/%s machine item %d stock %d price %d owner %d ready in %ld s\n", p->name().c_str(), field(text, "buycount").c_str(), field(text, "verify").c_str(), field(text, "expectprice").c_str(), field(text, "expectitem").c_str(), v.item, v.stock, price, v.owner, (long)(v.ready - now_s()));
    if (!field(text, "expectprice").empty()) // a visitor's answer: the real server asks twice (amount, then a confirmation)
    {
        if (v.owner == a.user_id) { console(p, "`oThis is your own machine: wrench it to restock or collect.``"); return; }
        int n = atoi(field(text, "buycount").c_str());
        if (v.ready > now_s()) { call(p->peer, {"OnTalkBubble", U32{(u32)p->netid}, "`9WARMING UP!``\nReady in : " + std::to_string(v.ready - now_s()) + " sec", U32{0}, U32{0}}); return; }
        if (!vend_usable(v, price)) { bubble(p, p, "`wOUT OF ORDER``"); return; }
        if (atoi(field(text, "expectprice").c_str()) != price || atoi(field(text, "expectitem").c_str()) != v.item) { console(p, "`4The machine changed, try again.``"); vend_buy_dialog(p, w, x, y); return; }
        n = std::min(n, v.stock);
        if (price < 0)   // whole packs: n items cost n / pack World Locks, rounded down to full packs
        {
            int pack = -price, packs = std::min(n / pack, (int)wl_total(a));
            if (n < pack) { bubble(p, p, "`4You have to buy at least " + std::to_string(pack) + " at a time.``"); return; }
            if (packs < 1) { bubble(p, p, "`4You can't afford that.``"); console(p, "`4You can't afford that: a pack costs `w1`` World Lock.``"); return; }
            n = packs * pack;
        }
        int cost = price > 0 ? price * n : n / (-price); const Item *it = S.items.get(v.item);
        if (n < 1) { bubble(p, p, "`4Enter how many you want to buy.``"); vend_buy_dialog(p, w, x, y); return; }
        if (wl_total(a) < cost) { bubble(p, p, "`4You can't afford that.``"); console(p, "`4You can't afford that: you need `w" + std::to_string(cost) + "`` World Locks.``"); return; }
        if ((!inv_count(a, v.item) && a.inv.size() >= a.backpack) || inv_count(a, v.item) + n > std::max<int>(1, it->max_amount))
        { call(p->peer, {"OnTalkBubble", U32{(u32)p->netid}, "You don't have room in your backpack!", U32{0}, U32{1}}); console(p, "You don't have room in your backpack!"); return; }
        if (atoi(field(text, "verify").c_str()) != 1)
        {
            dialog(p, "set_default_color|`o\nadd_label_with_icon|big|`wPurchase Confirmation``|left|1366|\nadd_spacer|small|\nembed_data|tilex|" + std::to_string(x) + "\nembed_data|tiley|" + std::to_string(y) + "\nadd_spacer|small|\n"
                      "add_textbox|`4You'll give:``|left|\nadd_spacer|small|\nadd_label_with_icon|small|(`w" + std::to_string(cost) + "``) `8World Locks``|left|242|\nadd_spacer|small|\nadd_textbox|`2You'll get:``|left|\nadd_spacer|small|\n"
                      "add_label_with_icon|small|(`w" + std::to_string(n) + "``) `2" + it->name + "``|left|" + std::to_string(v.item) + "|\nadd_spacer|small|\nadd_spacer|small|\nadd_textbox|Are you sure you want to make this purchase?|left|\n"
                      "embed_data|verify|1\nembed_data|buycount|" + std::to_string(n) + "\nembed_data|expectprice|" + std::to_string(price) + "\nembed_data|expectitem|" + std::to_string(v.item) + "\nend_dialog|vending|Cancel|OK|\n");
            return;
        }
        pay_wl(p, cost);
        diary_found_seed(p, v.item);
        inv_add(a, v.item, n); { GamePacket g; g.type = 0x0d; g.itemID = v.item; g.buildRange = std::min(n, 255); send_packet(p->peer, g); }
        v.stock -= n; v.money += cost; if (v.stock <= 0) v.item = 0;
        line("`7[```9" + p->name() + " bought " + std::to_string(n) + " " + it->name + " for " + std::to_string(cost) + " World Locks.```7]``");
        sfx_at(p, p, "audio/cash_register.wav");
        { GamePacket g; g.type = 0x13; g.buildRange = 5; g.targetNetID = p->netid; g.itemID = 100; g.posX = x * 32 + 16.f; g.posY = y * 32 + 16.f; g.tileX = v.item ? v.item : it->id; g.tileY = n; send_packet(p->peer, g); }
        vend_store(w, *t, x, y, v, v.item ? price : 0);
        return;
    }
    if (v.owner != a.user_id && v.owner != 0) return; // only the owner restocks, empties or collects
    std::string pressed = field(text, "buttonClicked");
    if (pressed == "pullstock")
    {
        if (v.stock > 0 && v.item) { inv_add(a, v.item, v.stock); GamePacket g; g.type = 0x0d; g.itemID = v.item; g.buildRange = std::min(v.stock, 255); send_packet(p->peer, g); }
        v.stock = 0; v.item = 0; vend_store(w, *t, x, y, v, 0); vend_owner_dialog(p, w, x, y); return;
    }
    if (pressed == "pullmoney")
    {
        if (v.money > 0) give_wl(p, v.money);
        v.money = 0; vend_store(w, *t, x, y, v, price); vend_owner_dialog(p, w, x, y); return;
    }
    if (pressed == "upgradedigital") { console(p, "`4DigiVend upgrades aren't available on this server.``"); return; }
    std::string pick = field(text, "stockitem");
    if (!pick.empty())
    {
        int id = atoi(pick.c_str()); const Item *it = S.items.get(id); int have = inv_count(a, id);
        if (!it || have < 1 || (it->flags & 0x8000)) { console(p, "`4You can't put that in a Vending Machine.``"); return; }
        if (v.item && v.item != id && v.stock > 0) { console(p, "`4Empty the machine first.``"); return; }
        inv_remove(a, id, have); send_remove(p, id, have);
        v.item = id; v.stock += have;
        for (u16 &c : a.clothes) if (c == id) c = 0;
        line("`7[``" + p->name() + " put `2" + it->name + "`` in the Vending Machine.`7]``");
        vend_store(w, *t, x, y, v, price); vend_owner_dialog(p, w, x, y); return;
    }
    if (!field(text, "setprice").empty() && v.item)
    {
        int n = std::max(0, atoi(field(text, "setprice").c_str())); bool per_lock = field(text, "chk_perlock") == "1";
        const Item *it = S.items.get(v.item);
        console(p, "`9WARMING UP!``\nReady in : 15 secs.");
        if (n > 25000) { bubble(p, p, "Well, that's a silly price."); console(p, "Well, that's a silly price."); return; }
        if (per_lock && v.stock < n)   // as the real server: a pack bigger than the stock can't be priced
        { call(p->peer, {"OnTalkBubble", U32{(u32)p->netid}, "You don't have enough in there for anyone to buy at that price!", U32{0}, U32{1}}); console(p, "You don't have enough in there for anyone to buy at that price!"); return; }
        int np = per_lock ? -n : n;
        v.ready = now_s() + 15;
        vend_store(w, *t, x, y, v, np);
        line("`7[``" + p->name() + " changed the price of `2" + it->name + "`` to " + (per_lock ? "`6" + std::to_string(n) + " per World Lock.``" : "`5" + std::to_string(n) + " World Locks each.``") + "`7]``");
    }
}

// breaking a Vending Machine hands back its stock and earnings
static void vend_break(Player *p, World &w, Tile &t)
{
    Vend v = vend_of(t); Account &a = *p->acct;
    if (v.stock > 0 && v.item) { inv_add(a, v.item, v.stock); GamePacket g; g.type = 0x0d; g.itemID = v.item; g.buildRange = std::min(v.stock, 255); send_packet(p->peer, g); }
    if (v.money > 0) give_wl(p, v.money);
    t.aux.clear(); (void)w;
}

// ---------------------------------------------------------------- the quest phone
static void phone_dialog(Player *p)
{
    dialog(p, "set_default_color|`o\nadd_label_with_icon|big|`wCrazy Jim's Quest Emporium``|left|3902|\nembed_data|num|12345\nadd_popup_name|Telephone12345|\nembed_data|remote|1\n"
              "add_textbox|Hey there! Crazy Jim's Quest Emporium, open all day. Pick a quest and see what you can do for me.|left|\n"
              "add_button|chc0|Daily Quest|noflags|0|0|1\nadd_button|chc1|Goals|noflags|0|0|1\nadd_button|chc2|Epic Quests|noflags|0|0|0\nadd_button|chc3|Biweekly Soul Quests|noflags|0|0|1\nend_dialog|phonecall|Hang Up||\n");
}
// today's quest: two items (a seed and a block) that depend only on the date, so everyone sees the same one
static void daily_items(int &a, int &na, int &b, int &nb)
{
    i64 day = now_s() / 86400; std::mt19937 g((u32)day * 2654435761u);
    std::vector<int> seeds, blocks;
    for (const Item &i : S.items.list) { if (i.rarity > 12 || i.rarity < 1 || (i.flags & 0x04)) continue; if (i.type == A_SEED) seeds.push_back(i.id); else if (i.type == 18 || i.type == 17) blocks.push_back(i.id); }
    a = seeds[g() % seeds.size()]; b = blocks[g() % blocks.size()]; na = 100 + (int)(g() % 101); nb = 10 + (int)(g() % 20);
}
static void phone_return(Player *p, const std::string &text)
{
    std::string b = field(text, "buttonClicked"); Account &a = *p->acct;
    if (b == "chc0")
    {
        int ia, na, ib, nb; daily_items(ia, na, ib, nb);
        console(p, "The Daily Quest requires " + std::to_string(na) + " " + S.items.get(ia)->name + " and " + std::to_string(nb) + " " + S.items.get(ib)->name + ".");
        bool done = a.quest_day == (i64)(now_s() / 86400);
        std::string s = "set_default_color|`o\nadd_label_with_icon|big|`wCrazy Jim's Daily Quest``|left|3902|\nembed_data|num|-3\nadd_popup_name|Telephone-3|\nembed_data|remote|1\n"
            "add_textbox|I collect things, and today I'm after these:|left|\n"
            "add_label_with_icon|small|`2" + std::to_string(na) + " " + S.items.get(ia)->name + "|left|" + std::to_string(ia) + "|\nadd_smalltext|and|left|\nadd_label_with_icon|small|`2" + std::to_string(nb) + " " + S.items.get(ib)->name + "|left|" + std::to_string(ib) + "|\nadd_spacer|small|\n"
            "add_smalltext|Send them through the phone and I'll give you a `2Growtoken``. One per person, until midnight.|left|\nadd_spacer|small|\n"
            "add_smalltext|`6(You have " + std::to_string(inv_count(a, ia)) + " " + S.items.get(ia)->name + " and " + std::to_string(inv_count(a, ib)) + " " + S.items.get(ib)->name + ")``|left|\n";
        if (done) s += "add_smalltext|`4You already did today's quest!``|left|\n"; else s += "add_button|dq_turnin|Hand it over|noflags|0|0|\n";
        dialog_later(p, s + "end_dialog|phonecall|Hang Up||\n", 500);
    }
    else if (b == "dq_turnin")
    {
        int ia, na, ib, nb; daily_items(ia, na, ib, nb);
        if (a.quest_day == (i64)(now_s() / 86400)) return;
        if (inv_count(a, ia) < na || inv_count(a, ib) < nb) { console(p, "`4You don't have everything the quest needs.``"); return; }
        inv_remove(a, ia, na); send_remove(p, ia, na); inv_remove(a, ib, nb); send_remove(p, ib, nb);
        inv_add(a, 1486, 1); GamePacket g; g.type = 0x0d; g.itemID = 1486; g.buildRange = 1; send_packet(p->peer, g);
        a.quest_day = now_s() / 86400; sfx(p, "audio/success.wav"); console(p, "`2Crazy Jim gave you a Growtoken!``");
    }
    else if (b == "chc1" || b == "chc2" || b == "chc3") console(p, "`oCrazy Jim: \"That quest line isn't open on this server yet.\"``");
}

// ---------------------------------------------------------------- entry points called from main.cpp
static bool features_profile_button(Player *p, const std::string &b)
{
    if (b == "set_online_status") status_dialog(p);
    else if (b == "billboard_edit") billboard_dialog(p);
    else if (b == "wardrobe_customization") { send_presets(p); call(p->peer, {"OnDialogRequestRML", "show_wardrobe_main_ui"}); }
    else if (b == "notebook_edit") notebook_view(p, 0);
    else if (b == "my_worlds") worlds_tab(p, 0);
    else if (b == "alist") achievements_dialog(p);
    else if (b == "title_edit") dialog(p, "set_default_color|`o\nadd_label|big|No Titles Obtained|right\nadd_spacer|small|\nadd_button||OK|noflags|0|0|\nend_dialog|title_edit||\n");
    else if (b == "open_worldlock_storage") call(p->peer, {"OnDialogRequestRML", "show_world_lock_storage"});
    else if (b == "trade_scan") trade_history_dialog(p);
    else return false;
    return true;
}

static bool features_dialog(Player *p, const std::string &name, const std::string &text)
{
    Account &a = *p->acct; World *w = S.world_of(p); std::string pressed = field(text, "buttonClicked");
    int page = atoi(field(text, "pageNum").c_str());
    if (name == "set_online_status")
    {
        a.status = field(text, "checkbox_status_busy") == "1" ? 1 : field(text, "checkbox_status_away") == "1" ? 2 : 0;
    }
    else if (name == "billboard_edit")
    {
        std::string pick = field(text, "billboard_item");
        if (!pick.empty()) { int id = atoi(pick.c_str()); if (S.items.get(id) && inv_count(a, id) > 0) a.bb_item = id; billboard_dialog(p, a.bb_item); return true; }
        a.bb_show = field(text, "billboard_toggle") == "1"; a.bb_buying = field(text, "billboard_buying_toggle") == "1";
        a.bb_price = std::clamp(atoi(field(text, "setprice").c_str()), 0, 99999); a.bb_peritem = field(text, "chk_perlock") == "1" ? 0 : 1;
        if (w) broadcast(p->world, [&](Player *q) { send_billboard(q, p); });
    }
    else if (name == "paginated_personal_notebook_view")
    {
        if (pressed.rfind("viewPnPage_", 0) == 0) notebook_view(p, atoi(pressed.c_str() + 11));
        else if (pressed == "editPnPage") notebook_edit(p, page);
        else if (pressed == "back") profile_self(p);
    }
    else if (name == "paginated_personal_notebook_edit")
    {
        page = std::clamp(page, 0, 4);
        if (pressed == "save") { std::string t = field(text, "personal_note"); a.notes[page] = t.substr(0, 256); }
        else if (pressed == "clear") a.notes[page].clear();
        notebook_view(p, page);
    }
    else if (name == "worlds_list")
    {
        if (pressed.rfind("myWorldsUiTab_", 0) == 0) worlds_tab(p, atoi(pressed.c_str() + 14));
        else if (!pressed.empty() && valid_world_name(pressed)) join_world(p, pressed);
        else profile_self(p);
    }
    else if (name == "alist") profile_self(p);
    else if (name == "title_edit")
    {
        call(p->peer, {"OnNameChanged", shown_name(w, p), "{\"PlayerWorldID\":" + std::to_string(p->netid) + ",\"WrenchCustomization\":{\"WrenchForegroundCanRotate\":false,\"WrenchForegroundID\":-1,\"WrenchIconID\":-1}}"}, p->netid);
        call(p->peer, {"OnCountryState", "us"}, p->netid);
    }
    else if (name == "socialportal")
    {
        if (pressed == "showfriend") friends_page(p, false);
        else if (pressed == "communityhub") console(p, "`oThe Community Hub isn't available on this server.``");
        else if (pressed == "show_apprentices") dialog(p, res_dialog("apprentices"));
        else if (pressed == "showguild") dialog(p, "set_default_color|`o\nadd_label_with_icon|big| `wGuild Home`` |left|8276|\nadd_spacer|small|\nadd_textbox|`oYou are not in a guild. Guilds aren't available on this server yet.``|left|\nadd_spacer|small|\nadd_button|socialportal|Back|noflags|0|0|\nadd_button||Close|noflags|0|0|\nadd_quick_exit|\nend_dialog|playerportal|||\n");
        else if (pressed == "trade_history") trade_history_dialog(p);
    }
    else if (name == "playerportal") social_portal(p);
    else if (name == "show_mentees" || name == "show_mentorhelp")
    {
        if (pressed == "rewards_UI") dialog(p, res_dialog("mentor_rewards"));
        else if (pressed == "helpmentor_UI") dialog(p, res_dialog("mentor_help"));
        else if (pressed == "back2_apprentices") dialog(p, res_dialog("apprentices"));
        else if (pressed == "back") social_portal(p);
    }
    else if (name == "friends")
    {
        if (pressed == "friend_all") friends_page(p, true);
        else if (pressed == "all_friends") friends_dialog(p);
        else if (pressed == "friends_options") console(p, "`oFriend options aren't available on this server yet.``");
        else if (pressed == "back") social_portal(p);
        else if (!pressed.empty() && isdigit((unsigned char)pressed[0]))
        {
            i32 uid = atoi(pressed.c_str()); std::string nm; { std::lock_guard g(S.store.mu); nm = S.store.name_of(uid); }
            Dialog d; d.label_icon("`w" + nm + "``", 18).spacer(); if (online_user(uid)) d.s += "add_button|warp_" + std::to_string(uid) + "|`wWarp to them``|noflags|0|0|\n";
            d.s += "add_button|remove_" + std::to_string(uid) + "|`4Remove friend``|noflags|0|0|\n"; d.quick_exit();
            dialog(p, d.end("friends_menu", "", "OK"));
        }
    }
    else if (name == "trade_confirm") trade_confirm_return(p, pressed);
    else if (name == "phonecall") phone_return(p, text);
    else if (w && (name == "vending")) vend_dialog_return(p, *w, name, text);
    else return false;
    return true;
}

static bool features_action(Player *p, const std::string &action, const std::string &text)
{
    if (action.rfind("wardrobe_", 0) == 0) wardrobe_action(p, action, text);
    else if (action == "worldlock_storage_getpage") call(p->peer, {"OnWorldLockStorageResponse", std::string("\x95\x90\x00\x00\x00\x00", 6)});
    else if (action == "tutorialTaskPhoneClicked") phone_dialog(p);
    else if (action == "validate_world")
    {
        std::string n = upper(clean_name(field(text, "name"))); int avail = 0;
        if (valid_world_name(n)) { auto it = S.worlds.find(n); World tmp; if (it != S.worlds.end()) avail = it->second.owner != 0; else if (tmp.load(S.cfg.data_dir + "/worlds", n)) avail = tmp.owner != 0; }
        send_text(p->peer, GAME_MSG, "action|world_validated|available|" + std::to_string(avail) + "|world_name|" + field(text, "name"));
    }
    else return false;
    return true;
}

// redraws every Vending Machine tile of a freshly loaded world from its saved stock and price
static void vend_refresh(World &w)
{
    for (int i = 0; i < (int)w.tiles.size(); ++i)
    {
        Tile &t = w.tiles[i]; const Item *it = S.items.get(t.fg);
        if (!it || it->type != 62) continue;
        Vend v = vend_of(t); int price = vend_price(t);
        Buf b; b.put<u8>(0x18).put<u32>(vend_usable(v, price) ? v.item : 0).put<i32>(price); t.extra = b.d; t.flags |= T_EXTRA;
        if (v.stock > 0) t.flags |= T_ON; else t.flags &= ~T_ON;
        t.aux = std::to_string(v.stock) + " " + std::to_string(v.money) + " " + std::to_string(v.ready) + " " + std::to_string(v.owner) + " " + std::to_string(v.item); // keeps the item even while the tile shows 0
    }
}
