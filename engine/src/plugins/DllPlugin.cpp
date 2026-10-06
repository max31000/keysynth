#include "plugins/DllPlugin.h"

#include "core/AppPaths.h"
#include "plugins/FaustParamMap.h"
#include "plugins/FpuGuard.h"
#include "platform/DynamicLibrary.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

namespace fs = std::filesystem;

namespace ks::plugins {

// ks_event must be bit-identical to MidiEvent: events are passed without copying.
static_assert(sizeof(ks_event) == sizeof(MidiEvent));
static_assert(offsetof(ks_event, sample_offset) == offsetof(MidiEvent, sampleOffset));
static_assert(offsetof(ks_event, type) == offsetof(MidiEvent, type));
static_assert(offsetof(ks_event, channel) == offsetof(MidiEvent, channel));
static_assert(offsetof(ks_event, data1) == offsetof(MidiEvent, data1));
static_assert(offsetof(ks_event, value7) == offsetof(MidiEvent, value7));
static_assert(offsetof(ks_event, value_f) == offsetof(MidiEvent, valueF));
static_assert(offsetof(ks_event, note_id) == offsetof(MidiEvent, noteId));
static_assert(static_cast<int>(MidiEventType::NoteOn) == KS_EV_NOTE_ON);
static_assert(static_cast<int>(MidiEventType::PitchBend) == KS_EV_PITCH_BEND);
static_assert(static_cast<int>(MidiEventType::AllNotesOff) == KS_EV_ALL_NOTES_OFF);
static_assert(static_cast<int>(MidiEventType::AllSoundOff) == KS_EV_ALL_SOUND_OFF);

namespace {

constexpr size_t kMinDescriptorSize = offsetof(ks_plugin_descriptor, load_state) + sizeof(void*);
constexpr size_t kMinParamSpecSize = offsetof(ks_param_spec, num_choices) + sizeof(uint32_t);
constexpr uint32_t kMaxParams = 256;
constexpr uint32_t kMaxChoices = 128;

// Bounded copy of a C string owned by the plugin (null-safe, never reads past `max`).
std::string cstr(const char* s, size_t max = 256) {
    if (!s) return {};
    size_t n = 0;
    while (n < max && s[n] != 0) ++n;
    return std::string(s, n);
}

std::atomic<uint32_t>& copyCounter() {
    static std::atomic<uint32_t> c{0};
    return c;
}

} // namespace

// --- PluginLibrary -------------------------------------------------------------------------------------------

std::shared_ptr<PluginLibrary> PluginLibrary::load(const fs::path& dll, const fs::path& tempDir, std::string& error) {
    std::error_code ec;
    fs::create_directories(tempDir, ec);
    const fs::path copy =
        tempDir / (pathToUtf8(dll.stem()) + "-" + std::to_string(copyCounter().fetch_add(1) + 1) + ".dll");
    fs::copy_file(dll, copy, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "cannot copy " + pathToUtf8(dll) + " to temp: " + ec.message();
        return nullptr;
    }
    std::shared_ptr<PluginLibrary> lib(new PluginLibrary());
    lib->source_ = dll;
    lib->copy_ = copy;
    lib->handle_ = platform::openLibrary(copy, error);
    if (!lib->handle_) {
        error = "cannot load " + pathToUtf8(dll.filename()) + ": " + error;
        return nullptr; // destructor removes the copy
    }
    auto get = reinterpret_cast<ks_get_plugin_fn>(platform::findSymbol(lib->handle_, "ks_get_plugin"));
    if (!get) {
        error = pathToUtf8(dll.filename()) + " does not export ks_get_plugin";
        return nullptr;
    }
    struct Ctx {
        ks_get_plugin_fn get;
        const ks_plugin_descriptor* d;
    } ctx{get, nullptr};
    uint32_t code = 0;
    if (!platform::guardedCall([](void* p) { auto* c = static_cast<Ctx*>(p); c->d = c->get(); }, &ctx, &code)) {
        error = "ks_get_plugin crashed (exception 0x" + [&] {
            char b[16];
            std::snprintf(b, sizeof b, "%08X", code);
            return std::string(b);
        }() + ")";
        return nullptr;
    }
    if (!ctx.d) {
        error = "ks_get_plugin returned null";
        return nullptr;
    }
    if (ctx.d->struct_size < kMinDescriptorSize) {
        error = "descriptor too small (struct_size " + std::to_string(ctx.d->struct_size) + ")";
        return nullptr;
    }
    if (ctx.d->abi_version != KS_PLUGIN_ABI_VERSION) {
        error = "unsupported abi_version " + std::to_string(ctx.d->abi_version) + " (host: " +
                std::to_string(KS_PLUGIN_ABI_VERSION) + ")";
        return nullptr;
    }
    std::memset(&lib->desc_, 0, sizeof lib->desc_);
    std::memcpy(&lib->desc_, ctx.d, std::min<size_t>(ctx.d->struct_size, sizeof lib->desc_));
    return lib;
}

PluginLibrary::~PluginLibrary() {
    if (handle_) {
        struct Ctx {
            void* h;
        } c{handle_};
        // DllMain(DLL_PROCESS_DETACH) of a broken plugin must not take the engine down either.
        platform::guardedCall([](void* p) { platform::closeLibrary(static_cast<Ctx*>(p)->h); }, &c);
    }
    std::error_code ec;
    if (!copy_.empty()) fs::remove(copy_, ec);
}

// --- DllPluginVersion ----------------------------------------------------------------------------------------

std::shared_ptr<DllPluginVersion> DllPluginVersion::create(const std::string& name, std::shared_ptr<PluginLibrary> lib,
                                                           std::string& error) {
    const ks_plugin_descriptor& d = lib->descriptor();
    if (!d.create || !d.destroy || !d.prepare || !d.reset || !d.process || !d.set_param) {
        error = "descriptor lacks a required function (create/destroy/prepare/reset/process/set_param)";
        return nullptr;
    }
    const std::string id = cstr(d.id, 64);
    if (id != name) {
        error = "descriptor id '" + id + "' must equal the plugin directory name '" + name + "'";
        return nullptr;
    }
    if (d.kind != KS_KIND_INSTRUMENT && d.kind != KS_KIND_EFFECT) {
        error = "invalid kind " + std::to_string(d.kind);
        return nullptr;
    }
    if (d.num_params > kMaxParams || (d.num_params > 0 && !d.params)) {
        error = "invalid params array (num_params " + std::to_string(d.num_params) + ")";
        return nullptr;
    }
    auto v = std::make_shared<DllPluginVersion>();
    v->name = name;
    v->source = PluginSource::Dll;
    v->lib = std::move(lib);
    ModuleInfo& info = v->info;
    info.typeId = "plugin:" + name;
    info.displayName = cstr(d.name, 128);
    if (info.displayName.empty()) info.displayName = name;
    info.kind = d.kind == KS_KIND_INSTRUMENT ? ModuleKind::Instrument : ModuleKind::Effect;
    info.category = cstr(d.category, 64);
    if (info.category.empty()) info.category = "Plugin";
    std::vector<std::string> groups;
    for (uint32_t i = 0; i < d.num_params; ++i) {
        const ks_param_spec& s = d.params[i];
        if (s.struct_size < kMinParamSpecSize) {
            error = "param " + std::to_string(i) + ": struct_size too small";
            return nullptr;
        }
        ParamSpec p;
        p.id = cstr(s.id, 64);
        if (!isValidParamId(p.id)) {
            error = "param " + std::to_string(i) + ": invalid id '" + p.id + "' (expected [a-z][a-z0-9_]*)";
            return nullptr;
        }
        for (const auto& q : info.params)
            if (q.id == p.id) {
                error = "duplicate param id '" + p.id + "'";
                return nullptr;
            }
        p.name = cstr(s.name, 128);
        if (p.name.empty()) p.name = p.id;
        p.group = cstr(s.group, 64);
        p.unit = cstr(s.unit, 16);
        if (!std::isfinite(s.min) || !std::isfinite(s.max) || !std::isfinite(s.def) || !(s.max > s.min)) {
            error = "param '" + p.id + "': invalid range";
            return nullptr;
        }
        p.min = s.min;
        p.max = s.max;
        p.def = std::clamp(s.def, s.min, s.max);
        if (s.scale > KS_SCALE_BOOL) {
            error = "param '" + p.id + "': invalid scale";
            return nullptr;
        }
        p.scale = static_cast<ParamScale>(s.scale);
        if (p.scale == ParamScale::Log && p.min <= 0.0f) p.scale = ParamScale::Linear;
        p.skewCentre = std::isfinite(s.skew_centre) ? s.skew_centre : 0.0f;
        p.flags = s.flags & (ParamFlags::ReadOnly | ParamFlags::Hidden | ParamFlags::NonAutomatable);
        if (p.scale == ParamScale::Enum) {
            if (!s.choices || s.num_choices == 0 || s.num_choices > kMaxChoices) {
                error = "param '" + p.id + "': enum needs 1.." + std::to_string(kMaxChoices) + " choices";
                return nullptr;
            }
            for (uint32_t c = 0; c < s.num_choices; ++c) p.choices.push_back(cstr(s.choices[c], 64));
            p.min = 0.0f;
            p.max = static_cast<float>(s.num_choices - 1);
            p.def = std::clamp(std::round(p.def), p.min, p.max);
        }
        if (!p.group.empty() && std::find(groups.begin(), groups.end(), p.group) == groups.end()) groups.push_back(p.group);
        info.params.push_back(std::move(p));
    }
    if (!groups.empty()) info.uiHints["groupOrder"] = groups;
    return v;
}

std::unique_ptr<Module> DllPluginVersion::createModule() const {
    auto m = std::make_unique<DllModule>(std::static_pointer_cast<const DllPluginVersion>(shared_from_this()));
    if (!m->valid()) return nullptr;
    return m;
}

// --- DllModule -----------------------------------------------------------------------------------------------

DllModule::DllModule(std::shared_ptr<const DllPluginVersion> v)
    : Module(v->info), v_(std::move(v)), d_(&v_->desc()) {
    struct Ctx {
        const ks_plugin_descriptor* d;
        ks_instance inst;
    } c{d_, nullptr};
    FpuGuard fpu;
    uint32_t code = 0;
    if (!platform::guardedCall([](void* p) { auto* x = static_cast<Ctx*>(p); x->inst = x->d->create(); }, &c, &code)) {
        fpu.restore();
        v_->reportFault(code);
        return;
    }
    inst_ = c.inst;
    lastSent_.assign(static_cast<size_t>(params().size()), std::numeric_limits<float>::quiet_NaN());
}

DllModule::~DllModule() {
    if (!inst_) return;
    struct Ctx {
        const ks_plugin_descriptor* d;
        ks_instance inst;
    } c{d_, inst_};
    FpuGuard fpu;
    platform::guardedCall([](void* p) { auto* x = static_cast<Ctx*>(p); x->d->destroy(x->inst); }, &c);
    inst_ = nullptr;
    // v_ (and with it possibly the library) is released after this body: destroy() runs before FreeLibrary.
}

void DllModule::fault(uint32_t code) noexcept {
    faulted_ = true;
    v_->reportFault(code);
}

void DllModule::prepare(double sampleRate, int maxBlock) {
    if (!inst_ || faulted_) return;
    struct Ctx {
        DllModule* m;
        double sr;
        int mb;
    } c{this, sampleRate, maxBlock};
    FpuGuard fpu;
    uint32_t code = 0;
    const bool ok = platform::guardedCall(
        [](void* p) {
            auto* x = static_cast<Ctx*>(p);
            const ks_plugin_descriptor* d = x->m->d_;
            d->prepare(x->m->inst_, x->sr, static_cast<int32_t>(x->mb));
            // Initial param values (later changes are sent from the audio thread at block start).
            for (int i = 0; i < x->m->params().size(); ++i) {
                if (x->m->params().spec(i).isReadOnly()) continue;
                const float val = x->m->params().get(i);
                d->set_param(x->m->inst_, static_cast<uint32_t>(i), val);
                x->m->lastSent_[static_cast<size_t>(i)] = val;
            }
            x->m->tail_ = d->tail_samples ? std::max(0, d->tail_samples(x->m->inst_)) : 0;
            x->m->latency_ = d->latency_samples ? std::max(0, d->latency_samples(x->m->inst_)) : 0;
        },
        &c, &code);
    if (!ok) {
        fpu.restore();
        fault(code);
    }
}

void DllModule::reset() {
    if (!inst_ || faulted_) return;
    struct Ctx {
        const ks_plugin_descriptor* d;
        ks_instance inst;
    } c{d_, inst_};
    FpuGuard fpu;
    uint32_t code = 0;
    if (!platform::guardedCall([](void* p) { auto* x = static_cast<Ctx*>(p); x->d->reset(x->inst); }, &c, &code)) {
        fpu.restore();
        fault(code);
    }
}

void DllModule::guardedProcess(void* self) {
    auto* m = static_cast<DllModule*>(self);
    const ks_plugin_descriptor* d = m->d_;
    const ParamSet& ps = m->params();
    for (int i = 0; i < ps.size(); ++i) {
        if (ps.spec(i).isReadOnly()) continue;
        const float val = ps.get(i);
        float& last = m->lastSent_[static_cast<size_t>(i)];
        if (val != last) { // NaN-initialized: always sent once
            d->set_param(m->inst_, static_cast<uint32_t>(i), val);
            last = val;
        }
    }
    float* stereo[2] = {m->cur_->left, m->cur_->right};
    d->process(m->inst_, stereo, static_cast<int32_t>(m->cur_->numSamples),
               reinterpret_cast<const ks_event*>(m->curEvents_.data()), static_cast<int32_t>(m->curEvents_.size()));
    if (d->get_param)
        for (int i = 0; i < ps.size(); ++i)
            if (ps.spec(i).isReadOnly()) m->params().setRaw(i, d->get_param(m->inst_, static_cast<uint32_t>(i)));
    if (d->tail_samples) m->tail_ = std::max(0, d->tail_samples(m->inst_));
}

void DllModule::process(AudioBlock& stereo, MidiEventSpan events, const ProcessContext&) {
    if (!inst_ || faulted_) {
        stereo.clear(); // muted (instrument buffers are already clear; effects go silent, not passthrough)
        return;
    }
    cur_ = &stereo;
    curEvents_ = events;
    FpuGuard fpu;
    uint32_t code = 0;
    if (!platform::guardedCall(&DllModule::guardedProcess, this, &code)) {
        fpu.restore();
        fault(code);
        stereo.clear();
        return;
    }
    // NaN/Inf guard (cheap): zero the block, keep running.
    float acc = 0.0f;
    for (int i = 0; i < stereo.numSamples; ++i) acc += (stereo.left[i] + stereo.right[i]) * 0.0f;
    if (acc != 0.0f) {
        stereo.clear();
        v_->reportFault(kFaultNonFinite);
    }
}

nlohmann::json DllModule::saveState() const {
    if (!inst_ || faulted_ || !d_->save_state) return nullptr;
    struct Ctx {
        const ks_plugin_descriptor* d;
        ks_instance inst;
        std::vector<unsigned char>* buf;
        int32_t n;
    };
    std::vector<unsigned char> buf;
    Ctx c{d_, inst_, &buf, 0};
    FpuGuard fpu;
    const bool ok = platform::guardedCall(
        [](void* p) {
            auto* x = static_cast<Ctx*>(p);
            x->n = x->d->save_state(x->inst, nullptr, 0);
            if (x->n <= 0 || x->n > (16 << 20)) return;
            x->buf->resize(static_cast<size_t>(x->n));
            const int32_t w = x->d->save_state(x->inst, x->buf->data(), x->n);
            if (w != x->n) x->n = -1;
        },
        &c);
    fpu.restore();
    if (!ok || c.n <= 0) return nullptr;
    return {{"blob", base64Encode(buf)}};
}

void DllModule::loadState(const nlohmann::json& state) {
    if (!inst_ || faulted_ || !d_->load_state || !state.is_object()) return;
    auto it = state.find("blob");
    if (it == state.end() || !it->is_string()) return;
    std::vector<unsigned char> data;
    if (!base64Decode(it->get<std::string>(), data) || data.empty()) return;
    struct Ctx {
        const ks_plugin_descriptor* d;
        ks_instance inst;
        const std::vector<unsigned char>* data;
    } c{d_, inst_, &data};
    FpuGuard fpu;
    uint32_t code = 0;
    if (!platform::guardedCall(
            [](void* p) {
                auto* x = static_cast<Ctx*>(p);
                x->d->load_state(x->inst, x->data->data(), static_cast<int32_t>(x->data->size()));
            },
            &c, &code)) {
        fpu.restore();
        fault(code);
    }
}

std::string DllModule::base64Encode(const std::vector<unsigned char>& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | in[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
    }
    if (i < in.size()) {
        uint32_t v = uint32_t(in[i]) << 16;
        if (i + 1 < in.size()) v |= uint32_t(in[i + 1]) << 8;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += i + 1 < in.size() ? tbl[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

bool DllModule::base64Decode(const std::string& s, std::vector<unsigned char>& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    if (s.size() % 4 != 0) return false;
    for (size_t i = 0; i < s.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i + static_cast<size_t>(k)];
            if (c == '=' && i + 4 == s.size() && k >= 2) {
                v[k] = 0;
                ++pad;
            } else {
                v[k] = val(c);
                if (v[k] < 0 || pad) return false;
            }
        }
        const uint32_t x = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) | (uint32_t(v[2]) << 6) | uint32_t(v[3]);
        out.push_back(static_cast<unsigned char>(x >> 16));
        if (pad < 2) out.push_back(static_cast<unsigned char>((x >> 8) & 0xFF));
        if (pad < 1) out.push_back(static_cast<unsigned char>(x & 0xFF));
    }
    return true;
}

} // namespace ks::plugins
