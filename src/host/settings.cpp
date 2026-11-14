#include "host/settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace personarm::host {

namespace {
using Warnings = std::vector<std::string>*;
void Warn(Warnings w, const std::string& m) { if (w) w->push_back(m); }

const JsonValue* Section(const JsonValue* j, const char* k) {
    const JsonValue* v = j ? j->Find(k) : nullptr;
    return v && v->IsObject() ? v : nullptr;
}
bool Missing(const JsonValue* o, const char* k) { return !o || !o->Has(k); }

void GetEnum(const JsonValue* o, const std::string& sect, const char* k, std::initializer_list<const char*> ok,
             std::string& out, Warnings w) {
    if (Missing(o, k)) return;
    const JsonValue& v = *o->Find(k);
    if (v.IsString())
        for (const char* c : ok) if (v.AsString() == c) { out = c; return; }
    Warn(w, sect + "." + k + ": not one of the documented values, using \"" + out + "\"");
}
void GetInt(const JsonValue* o, const char* sect, const char* k, int lo, int hi, int& out, Warnings w) {
    if (Missing(o, k)) return;
    const JsonValue& v = *o->Find(k);
    const double d = v.AsNumber(-1e9);
    if (v.IsNumber() && d >= lo && d <= hi && d == double(int(d))) { out = int(d); return; }
    Warn(w, std::string(sect) + "." + k + ": expected an integer " + std::to_string(lo) + ".." + std::to_string(hi) +
                ", using " + std::to_string(out));
}
void GetBool(const JsonValue* o, const char* sect, const char* k, bool& out, Warnings w) {
    if (Missing(o, k)) return;
    if (o->Find(k)->IsBool()) out = o->Find(k)->AsBool();
    else Warn(w, std::string(sect) + "." + k + ": expected true/false, using " + (out ? "true" : "false"));
}
void GetString(const JsonValue* o, const std::string& sect, const char* k, std::string& out, Warnings w) {
    if (Missing(o, k)) return;
    if (o->Find(k)->IsString()) out = o->Find(k)->AsString();
    else Warn(w, sect + "." + k + ": expected a string, ignored");
}

const char* ActionOfBit(uint16_t bit) {
    for (const NamedBit& a : ActionNames()) if (a.bit == bit) return a.name;
    return "";
}
using BindMap = std::map<std::string, std::vector<std::string>>;
void DefaultBindings(const std::vector<NamedBit>& table, BindMap& dst) {
    for (const NamedBit& m : table) dst[ActionOfBit(m.bit)].push_back(m.name);
}
void ParseBindings(const JsonValue* sect, const char* name, BindMap& dst, Warnings w) {
    if (!sect) return;
    for (const auto& [action, list] : sect->Members()) {
        const std::string where = std::string("input.") + name + "." + action;
        if (!PadBitForName(ActionNames(), action)) { Warn(w, where + ": unknown action, kept untouched"); continue; }
        if (!list.IsArray()) { Warn(w, where + ": expected a list of names, using the default"); continue; }
        std::vector<std::string> names;
        for (const JsonValue& n : list.Items()) {
            if (n.IsString() && !n.AsString().empty()) names.push_back(n.AsString());
            else Warn(w, where + ": entry is not a non-empty string, skipped");
        }
        dst[action] = names;
    }
}

JsonValue Sub(const JsonValue& doc, const std::string& k) {
    const JsonValue* v = doc.Find(k);
    return v && v->IsObject() ? *v : JsonValue::MakeObject();
}
JsonValue StringList(const std::vector<std::string>& l) {
    JsonValue a = JsonValue::MakeArray();
    for (const std::string& s : l) a.Push(s);
    return a;
}
} // namespace

Settings DefaultSettings() {
    Settings s;
    DefaultBindings(KeyboardMap(), s.keyboard);
    DefaultBindings(GamepadMap(), s.gamepad);
    return s;
}

Settings ParseSettings(const std::string& text, std::vector<std::string>* w) {
    Settings s = DefaultSettings();
    std::string err;
    std::optional<JsonValue> doc = JsonValue::Parse(text, &err);
    if (!doc || !doc->IsObject()) {
        Warn(w, doc ? "settings: top level is not an object, using defaults" : "settings: " + err + ", using defaults");
        return s;
    }
    s.raw = *doc;
    if (const JsonValue* v = doc->Find("version")) {
        if (v->IsNumber() && v->AsNumber() >= 1) s.version = int(v->AsNumber());
        else Warn(w, "version: expected a positive integer, assuming 1");
        if (s.version > kSettingsVersion)
            Warn(w, "version " + std::to_string(s.version) + " is newer than this host (" +
                        std::to_string(kSettingsVersion) + "): unknown keys are kept, known ones read as-is");
    }
    const JsonValue* video = Section(&*doc, "video");
    GetEnum(video, "video", "window_mode", {"windowed", "fullscreen", "borderless"}, s.video.window_mode, w);
    GetInt(video, "video", "window_scale", 1, 8, s.video.window_scale, w);
    GetBool(video, "video", "integer_scaling", s.video.integer_scaling, w);
    GetEnum(video, "video", "aspect", {"4:3", "stretch"}, s.video.aspect, w);
    GetBool(video, "video", "vsync", s.video.vsync, w);
    GetInt(video, "video", "resolution_scale", 1, 4, s.video.resolution_scale, w);
    GetInt(video, "video", "fps_target", 30, 120, s.video.fps_target, w);
    if (s.video.fps_target != 30 && s.video.fps_target != 60 && s.video.fps_target != 90 && s.video.fps_target != 120) {
        Warn(w, "video.fps_target: expected 30, 60, 90 or 120, using 60");
        s.video.fps_target = 60;
    }
    const JsonValue* audio = Section(&*doc, "audio");
    GetInt(audio, "audio", "master_volume", 0, 100, s.audio.master_volume, w);
    GetBool(audio, "audio", "mute", s.audio.mute, w);
    const JsonValue* general = Section(&*doc, "general");
    GetBool(general, "general", "pause_on_focus_loss", s.pause_on_focus_loss, w);
    GetString(general, "general", "language", s.language, w);
    GetString(general, "general", "save_dir", s.save_dir, w);
    if (s.language.empty()) s.language = "ja";
    const JsonValue* input = Section(&*doc, "input");
    ParseBindings(Section(input, "keyboard"), "keyboard", s.keyboard, w);
    ParseBindings(Section(input, "gamepad"), "gamepad", s.gamepad, w);
    if (const JsonValue* game = Section(&*doc, "game")) {
        for (const auto& [serial, g] : game->Members()) {
            const std::string where = "game." + serial;
            if (!g.IsObject()) { Warn(w, where + ": expected an object, ignored"); continue; }
            GameSettings& gs = s.games[serial];
            GetString(&g, where, "memcard1", gs.memcard[0], w);
            GetString(&g, where, "memcard2", gs.memcard[1], w);
            GetString(&g, where, "mods", gs.mods, w);
            GetEnum(&g, where, "text_speed", {"normal", "fast", "instant"}, gs.text_speed, w);
            if (!Missing(&g, "recomp")) {
                if (g.Find("recomp")->IsBool()) gs.recomp = g.Find("recomp")->AsBool();
                else Warn(w, where + ".recomp: expected true/false, ignored");
            }
        }
    }
    return s;
}

Settings LoadSettings(const std::string& path, std::vector<std::string>* w) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return DefaultSettings();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ParseSettings(ss.str(), w);
}

std::string DumpSettings(const Settings& s) {
    JsonValue doc = s.raw.IsObject() ? s.raw : JsonValue::MakeObject();
    doc.Set("version", s.version);
    JsonValue v = Sub(doc, "video");
    v.Set("window_mode", s.video.window_mode);
    v.Set("window_scale", s.video.window_scale);
    v.Set("integer_scaling", s.video.integer_scaling);
    v.Set("aspect", s.video.aspect);
    v.Set("vsync", s.video.vsync);
    v.Set("fps_target", s.video.fps_target);
    v.Set("resolution_scale", s.video.resolution_scale);
    doc.Set("video", v);
    JsonValue a = Sub(doc, "audio");
    a.Set("master_volume", s.audio.master_volume);
    a.Set("mute", s.audio.mute);
    doc.Set("audio", a);
    JsonValue g = Sub(doc, "general");
    g.Set("pause_on_focus_loss", s.pause_on_focus_loss);
    g.Set("language", s.language);
    g.Set("save_dir", s.save_dir);
    doc.Set("general", g);
    JsonValue in = Sub(doc, "input");
    JsonValue kb = Sub(in, "keyboard"), gp = Sub(in, "gamepad");
    for (const auto& [act, l] : s.keyboard) kb.Set(act, StringList(l));
    for (const auto& [act, l] : s.gamepad) gp.Set(act, StringList(l));
    in.Set("keyboard", kb);
    in.Set("gamepad", gp);
    doc.Set("input", in);
    JsonValue games = Sub(doc, "game");
    for (const auto& [serial, gs] : s.games) {
        if (serial.empty() || (gs.Empty() && !games.Has(serial))) continue;
        JsonValue o = Sub(games, serial);
        if (!gs.memcard[0].empty()) o.Set("memcard1", gs.memcard[0]);
        if (!gs.memcard[1].empty()) o.Set("memcard2", gs.memcard[1]);
        if (!gs.mods.empty()) o.Set("mods", gs.mods);
        if (!gs.text_speed.empty()) o.Set("text_speed", gs.text_speed);
        if (gs.recomp) o.Set("recomp", *gs.recomp);
        games.Set(serial, o);
    }
    doc.Set("game", games);
    return doc.Dump(2) + "\n";
}

bool SaveSettings(const std::string& path, const Settings& s, std::string* err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(path), tmp(path + ".tmp");
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << DumpSettings(s);
        out.flush();
        if (!out) { if (err) *err = "cannot write " + tmp.string(); out.close(); fs::remove(tmp, ec); return false; }
    }
    fs::rename(tmp, p, ec);  // replaces an existing file (MoveFileEx / rename(2))
    if (ec) { if (err) *err = "cannot replace " + path + ": " + ec.message(); fs::remove(tmp, ec); return false; }
    return true;
}

Settings ApplyCli(Settings s, const HostOptions& o, const std::string& serial) {
    if (!o.window_mode.empty()) s.video.window_mode = o.window_mode;
    if (o.window_scale > 0) s.video.window_scale = o.window_scale;
    if (o.interp_given) s.video.fps_target = o.interp_hz ? o.interp_hz : 60;
    if (o.volume >= 0) s.audio.master_volume = o.volume;
    if (o.mute) s.audio.mute = true;
    GameSettings g = GameFor(s, serial);
    for (int i = 0; i < 2; ++i) if (!o.memcard[i].empty()) g.memcard[i] = o.memcard[i];
    if (!o.text_speed.empty()) g.text_speed = o.text_speed;
    if (!o.mods.empty()) g.mods = o.mods;
    if (o.recomp) g.recomp = true;
    if (!g.Empty()) s.games[serial] = g;
    return s;
}

GameSettings GameFor(const Settings& s, const std::string& serial) {
    auto it = s.games.find(serial);
    return it == s.games.end() ? GameSettings{} : it->second;
}

std::string MemcardFor(const Settings& s, const GameSettings& g, const std::string& serial, int slot) {
    if (!g.memcard[slot].empty()) return g.memcard[slot];
    if (slot == 0 && !serial.empty() && !s.save_dir.empty())
        return (std::filesystem::path(s.save_dir) / (serial + "-1.mcd")).string();
    return "";
}

std::vector<Binding> FlattenBindings(const BindMap& m) {
    std::vector<Binding> out;
    for (const auto& [action, names] : m)
        if (uint16_t bit = PadBitForName(ActionNames(), action))
            for (const std::string& n : names) out.push_back({n, bit});
    return out;
}

} // namespace personarm::host
