#include "mf/package.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <set>

#include "mf/model.hpp"
#include "mf/storage.hpp"
#include "miniz.h"
#include "monocypher-ed25519.h"
#include "monocypher.h"

namespace mf {

std::string toHex(const uint8_t* d, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s += h[d[i] >> 4]; s += h[d[i] & 15]; }
    return s;
}
std::vector<uint8_t> fromHex(const std::string& s) {
    std::vector<uint8_t> out;
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        int a = v(s[i]), b = v(s[i + 1]);
        if (a < 0 || b < 0) return {};
        out.push_back((uint8_t)(a * 16 + b));
    }
    return out;
}

void randomBytes(uint8_t* out, size_t n) {
    int fd = ::open("/dev/urandom", O_RDONLY);
    size_t got = 0;
    if (fd >= 0) {
        while (got < n) {
            ssize_t r = ::read(fd, out + got, n - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        ::close(fd);
    }
    if (got < n) throw std::runtime_error("secure random source unavailable");
}

std::string blake2bHex(const uint8_t* data, size_t n) {
    uint8_t h[32];
    crypto_blake2b(h, 32, data, n);
    return toHex(h, 32);
}

std::string blake2bFileHex(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, 32);
    std::vector<uint8_t> buf(1 << 16);
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) crypto_blake2b_update(&ctx, buf.data(), n);
    std::fclose(f);
    uint8_t h[32];
    crypto_blake2b_final(&ctx, h);
    return toHex(h, 32);
}

void generateSigningKey(std::string& secretHex, std::string& publicHex) {
    uint8_t seed[32], sk[64], pk[32];
    randomBytes(seed, 32);
    crypto_ed25519_key_pair(sk, pk, seed);
    secretHex = toHex(sk, 64);
    publicHex = toHex(pk, 32);
    crypto_wipe(sk, 64);
}

std::string packageExtension(const std::string& kind) {
    if (kind == "project") return "mforge";
    if (kind == "script") return "mfsx";
    if (kind == "extension") return "mfext";
    if (kind == "preset") return "mffx";
    if (kind == "capsule") return "mfcapsule";
    if (kind == "template") return "mftemplate";
    return "zip";
}
std::string packageKindForExtension(const std::string& ext) {
    if (ext == "mforge") return "project";
    if (ext == "mfsx") return "script";
    if (ext == "mfext") return "extension";
    if (ext == "mffx") return "preset";
    if (ext == "mfcapsule") return "capsule";
    if (ext == "mftemplate") return "template";
    return {};
}

static const uint32_t kArgonBlocks = 16 * 1024;  // 16 MiB, mobile friendly
static const uint32_t kArgonPasses = 3;

static void deriveKey(const std::string& password, const uint8_t salt[16], uint32_t blocks, uint32_t passes, uint8_t key[32]) {
    std::vector<uint8_t> work((size_t)blocks * 1024);
    crypto_argon2_config cfg = {CRYPTO_ARGON2_I, blocks, passes, 1};
    crypto_argon2_inputs in = {(const uint8_t*)password.data(), salt, (uint32_t)password.size(), 16};
    crypto_argon2(key, 32, work.data(), cfg, in, crypto_argon2_no_extras);
}

static std::vector<uint8_t> sealEntry(const uint8_t key[32], const std::string& path, const std::vector<uint8_t>& plain) {
    std::vector<uint8_t> out(24 + 16 + plain.size());
    randomBytes(out.data(), 24);
    crypto_aead_lock(out.data() + 40, out.data() + 24, key, out.data(), (const uint8_t*)path.data(), path.size(), plain.data(), plain.size());
    return out;
}

static bool openEntry(const uint8_t key[32], const std::string& path, const uint8_t* data, size_t n, std::vector<uint8_t>& plain) {
    if (n < 40) return false;
    plain.resize(n - 40);
    return crypto_aead_unlock(plain.data(), data + 24, key, data, (const uint8_t*)path.data(), path.size(), data + 40, n - 40) == 0;
}

bool writePackage(const std::string& path, Package& pkg, const PackageWriteOptions& opt, std::string& err) {
    json& m = pkg.manifest;
    m["format"] = "motionforge-package";
    m["kind"] = pkg.kind;
    m["packageVersion"] = 1;
    m["formatVersion"] = kFormatVersion;
    m["engineVersion"] = kEngineVersion;
    m["compatibility"] = {{"engineMin", "1.0.0"}, {"engineMaxTested", "1.x"}};
    m["extensionApi"] = kExtensionApiVersion;
    m["created"] = nowSeconds();
    if (!opt.author.empty()) m["author"] = opt.author;
    json sums = json::object();
    for (auto& [p, d] : pkg.files) sums[p] = "blake2b:" + blake2bHex(d.data(), d.size());
    for (auto& [p, f] : pkg.diskFiles) {
        std::string h = blake2bFileHex(f);
        if (h.empty()) { err = "Cannot read file for packaging: " + pathBasename(f); return false; }
        sums[p] = "blake2b:" + h;
    }
    m["checksums"] = sums;
    uint8_t key[32];
    bool enc = !opt.password.empty();
    if (enc) {
        uint8_t salt[16];
        randomBytes(salt, 16);
        m["encryption"] = {{"alg", "xchacha20-poly1305"}, {"kdf", "argon2i"}, {"salt", toHex(salt, 16)}, {"blocks", kArgonBlocks}, {"passes", kArgonPasses}};
        deriveKey(opt.password, salt, kArgonBlocks, kArgonPasses, key);
    } else {
        m.erase("encryption");
    }
    std::string manifestText = m.dump(2);
    std::string tmp = path + ".partial";
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, tmp.c_str(), 0)) { err = "Cannot create package file (storage full or read-only?)."; return false; }
    bool ok = true;
    auto add = [&](const std::string& p, const std::vector<uint8_t>& d, bool compress) {
        if (!ok) return;
        if (!mz_zip_writer_add_mem(&zip, p.c_str(), d.data(), d.size(), compress ? MZ_DEFAULT_COMPRESSION : MZ_NO_COMPRESSION)) {
            ok = false;
            err = "Failed writing package entry " + p + ".";
        }
    };
    add("manifest.json", std::vector<uint8_t>(manifestText.begin(), manifestText.end()), true);
    if (!opt.signingKey.empty()) {
        auto sk = fromHex(opt.signingKey);
        if (sk.size() != 64) { err = "Signing key is invalid."; mz_zip_writer_end(&zip); ::unlink(tmp.c_str()); return false; }
        uint8_t sig[64];
        crypto_ed25519_sign(sig, sk.data(), (const uint8_t*)manifestText.data(), manifestText.size());
        std::vector<uint8_t> sigbin(sk.begin() + 32, sk.end());  // public key (second half of monocypher secret)
        sigbin.insert(sigbin.end(), sig, sig + 64);
        add("signature.bin", sigbin, false);
    }
    for (auto& [p, d] : pkg.files) add(p, enc ? sealEntry(key, p, d) : d, true);
    for (auto& [p, f] : pkg.diskFiles) {
        if (!ok) break;
        if (enc) {
            int64_t sz = fileSize(f);
            if (sz > (512ll << 20)) { ok = false; err = "Encrypted packages cannot contain files larger than 512 MB. Use Linked mode or no password."; break; }
            add(p, sealEntry(key, p, readFileBytes(f)), false);
        } else if (!mz_zip_writer_add_file(&zip, p.c_str(), f.c_str(), nullptr, 0, MZ_NO_COMPRESSION)) {
            ok = false;
            err = "Failed adding media " + pathBasename(f) + " to package.";
        }
    }
    if (enc) crypto_wipe(key, 32);
    if (ok && !mz_zip_writer_finalize_archive(&zip)) { ok = false; err = "Failed to finalize package."; }
    mz_zip_writer_end(&zip);
    if (!ok) { ::unlink(tmp.c_str()); return false; }
    if (::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); err = "Cannot move package into place."; return false; }
    return true;
}

bool isPackageEncrypted(const std::string& path) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) return false;
    int idx = mz_zip_reader_locate_file(&zip, "manifest.json", nullptr, 0);
    bool enc = false;
    if (idx >= 0) {
        size_t n;
        void* p = mz_zip_reader_extract_to_heap(&zip, idx, &n, 0);
        if (p) {
            json m = json::parse(std::string((char*)p, n), nullptr, false);
            enc = !m.is_discarded() && m.contains("encryption");
            mz_free(p);
        }
    }
    mz_zip_reader_end(&zip);
    return enc;
}

PackageReadResult readPackage(const std::string& path, const PackageReadOptions& opt) {
    PackageReadResult r;
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) { r.error = "Not a valid MOTIONFORGE package (unreadable archive)."; return r; }
    auto extract = [&](int idx, std::vector<uint8_t>& out) {
        size_t n;
        void* p = mz_zip_reader_extract_to_heap(&zip, idx, &n, 0);
        if (!p) return false;
        out.assign((uint8_t*)p, (uint8_t*)p + n);
        mz_free(p);
        return true;
    };
    int mi = mz_zip_reader_locate_file(&zip, "manifest.json", nullptr, 0);
    std::vector<uint8_t> mbytes;
    if (mi < 0 || !extract(mi, mbytes)) { mz_zip_reader_end(&zip); r.error = "Package has no manifest."; return r; }
    std::string manifestText(mbytes.begin(), mbytes.end());
    json m = json::parse(manifestText, nullptr, false);
    if (m.is_discarded() || m.value("format", "") != "motionforge-package") { mz_zip_reader_end(&zip); r.error = "Package manifest is invalid."; return r; }
    r.pkg.manifest = m;
    r.pkg.kind = m.value("kind", std::string());
    if (m.value("packageVersion", 1) > 1 || m.value("formatVersion", 1) > kFormatVersion) {
        mz_zip_reader_end(&zip);
        r.error = "Package was created by a newer version of MOTIONFORGE. Update the app to open it.";
        return r;
    }
    if (m.value("extensionApi", 1) > kExtensionApiVersion) r.warnings.push_back("Package targets a newer extension API; some features may be unavailable.");
    // Signature.
    int si = mz_zip_reader_locate_file(&zip, "signature.bin", nullptr, 0);
    if (si >= 0) {
        std::vector<uint8_t> sig;
        r.signedPkg = true;
        if (extract(si, sig) && sig.size() == 96) {
            r.signer = toHex(sig.data(), 32);
            r.signatureValid = crypto_ed25519_check(sig.data() + 32, sig.data(), (const uint8_t*)manifestText.data(), manifestText.size()) == 0;
            r.trusted = r.signatureValid && std::find(opt.trustedKeys.begin(), opt.trustedKeys.end(), r.signer) != opt.trustedKeys.end();
            r.revoked = std::find(opt.revokedKeys.begin(), opt.revokedKeys.end(), r.signer) != opt.revokedKeys.end();
        }
        if (!r.signatureValid) { mz_zip_reader_end(&zip); r.error = "Package signature is invalid; the package was modified after signing."; return r; }
        if (r.revoked) { mz_zip_reader_end(&zip); r.error = "Package was signed with a revoked key and cannot be opened."; return r; }
    }
    uint8_t key[32];
    r.encrypted = m.contains("encryption");
    if (r.encrypted) {
        if (opt.password.empty()) { mz_zip_reader_end(&zip); r.error = "This package is password protected."; return r; }
        auto salt = fromHex(m["encryption"].value("salt", std::string()));
        if (salt.size() != 16) { mz_zip_reader_end(&zip); r.error = "Encryption header is damaged."; return r; }
        deriveKey(opt.password, salt.data(), m["encryption"].value("blocks", kArgonBlocks), m["encryption"].value("passes", kArgonPasses), key);
    }
    const json& sums = jobj(m, "checksums");
    for (auto& [p, expected] : sums.items()) {
        int idx = mz_zip_reader_locate_file(&zip, p.c_str(), nullptr, 0);
        if (idx < 0) { r.error = "Package is incomplete: missing " + p + "."; break; }
        mz_zip_archive_file_stat st;
        mz_zip_reader_file_stat(&zip, idx, &st);
        bool large = !opt.extractDir.empty() && st.m_uncomp_size > (8u << 20) && !r.encrypted;
        if (large) {
            std::string out = pathJoin(opt.extractDir, pathBasename(p));
            makeDirs(opt.extractDir);
            if (!mz_zip_reader_extract_to_file(&zip, idx, out.c_str(), 0)) { r.error = "Failed extracting " + p + " (storage full?)."; break; }
            if ("blake2b:" + blake2bFileHex(out) != expected.get<std::string>()) { r.error = "Checksum mismatch for " + p + "; the package is damaged."; break; }
            r.extracted[p] = out;
            continue;
        }
        std::vector<uint8_t> raw;
        if (!extract(idx, raw)) { r.error = "Failed reading " + p + "."; break; }
        std::vector<uint8_t> plain;
        if (r.encrypted) {
            if (!openEntry(key, p, raw.data(), raw.size(), plain)) { r.error = "Wrong password, or the package is damaged."; break; }
        } else plain = std::move(raw);
        if ("blake2b:" + blake2bHex(plain.data(), plain.size()) != expected.get<std::string>()) { r.error = "Checksum mismatch for " + p + "; the package is damaged."; break; }
        if (!opt.extractDir.empty() && plain.size() > (8u << 20)) {
            std::string out = pathJoin(opt.extractDir, pathBasename(p));
            makeDirs(opt.extractDir);
            atomicWriteFile(out, std::string(plain.begin(), plain.end()));
            r.extracted[p] = out;
        } else {
            r.pkg.files[p] = std::move(plain);
        }
    }
    if (r.encrypted) crypto_wipe(key, 32);
    mz_zip_reader_end(&zip);
    if (!r.error.empty()) return r;
    if (!r.signedPkg) r.warnings.push_back("Package is unsigned.");
    r.ok = true;
    return r;
}

// ====================================================================== project packages
bool exportProjectPackage(const json& docIn, const std::string& outPath, ProjectPackMode mode, const std::function<std::string(const json&)>& assetPath,
                          const std::function<std::string(const std::string&)>& fontPath, const PackageWriteOptions& opt, std::string& err,
                          const std::string& thumbnailPng) {
    json doc = docIn;
    Package pkg;
    pkg.kind = "project";
    std::vector<std::string> notCollected;
    for (auto& a : doc["assets"]) {
        if (a.contains("generator")) continue;
        std::string p = assetPath ? assetPath(a) : std::string();
        if (!p.empty() && fileExists(p)) {
            if (!a.contains("checksum") || a["checksum"].get<std::string>().empty()) a["checksum"] = "blake2b:" + blake2bFileHex(p);
        }
        if (mode == ProjectPackMode::Linked) continue;
        if (p.empty() || !fileExists(p)) { notCollected.push_back(a.value("name", std::string())); continue; }
        std::string ext = pathExtensionLower(p);
        std::string entry = "assets/" + a.value("id", std::string()) + (ext.empty() ? "" : "." + ext);
        pkg.diskFiles[entry] = p;
        a["packagePath"] = entry;
    }
    if (mode == ProjectPackMode::Portable && fontPath) {
        std::set<std::string> fonts;
        for (auto& c : doc["comps"])
            for (auto& L : c["layers"]) {
                if (L.contains("text")) fonts.insert(L["text"].value("font", ""));
                if (L.contains("captions")) fonts.insert(jobj(L["captions"], "style").value("font", ""));
            }
        json embedded = json::array();
        for (auto& f : fonts) {
            std::string p = fontPath(f);
            if (p.empty() || !fileExists(p)) continue;
            pkg.diskFiles["fonts/" + f + "." + pathExtensionLower(p)] = p;
            embedded.push_back(f);
        }
        pkg.manifest["embeddedFonts"] = embedded;
        pkg.manifest["fontNotice"] = "Embedded fonts are included at the user's request; importing does not grant a font license.";
    }
    std::vector<uint8_t> cbor = json::to_cbor(doc);
    pkg.files["project.mfbin"] = cbor;
    if (!thumbnailPng.empty() && fileExists(thumbnailPng)) pkg.files["thumbnails/thumbnail.png"] = readFileBytes(thumbnailPng);
    for (auto& c : doc["comps"])
        for (auto& L : c["layers"])
            if (L.contains("captions") && !jarr(L["captions"], "items").empty()) {
                std::string s = L["captions"]["items"].dump();
                pkg.files["captions/" + L.value("id", std::string()) + ".json"] = std::vector<uint8_t>(s.begin(), s.end());
            }
    pkg.manifest["name"] = jobj(doc, "meta").value("name", std::string());
    pkg.manifest["mode"] = mode == ProjectPackMode::Linked ? "linked" : mode == ProjectPackMode::Collected ? "collected" : "portable";
    pkg.manifest["notCollected"] = notCollected;
    return writePackage(outPath, pkg, opt, err);
}

bool importProjectPackage(const std::string& path, const std::string& mediaDir, json& docOut, std::vector<std::string>& warnings,
                          const PackageReadOptions& opt0, std::string& err) {
    PackageReadOptions opt = opt0;
    opt.extractDir = mediaDir;
    PackageReadResult r = readPackage(path, opt);
    if (!r.ok) { err = r.error; return false; }
    if (r.pkg.kind != "project") { err = "This file is not a project package."; return false; }
    auto it = r.pkg.files.find("project.mfbin");
    if (it == r.pkg.files.end()) { err = "Project data missing from package."; return false; }
    json doc = json::from_cbor(it->second, true, false);
    if (doc.is_discarded()) { err = "Project data is damaged."; return false; }
    std::vector<std::string> log;
    try {
        doc = migrateProject(doc, &log);
    } catch (std::exception& e) {
        err = e.what();
        return false;
    }
    for (auto& l : log) warnings.push_back(l);
    makeDirs(mediaDir);
    for (auto& a : doc["assets"]) {
        std::string pp = a.value("packagePath", std::string());
        if (!pp.empty()) {
            std::string local;
            if (r.extracted.count(pp)) local = r.extracted[pp];
            else if (r.pkg.files.count(pp)) {
                local = pathJoin(mediaDir, pathBasename(pp));
                auto& d = r.pkg.files[pp];
                atomicWriteFile(local, std::string(d.begin(), d.end()));
            }
            if (!local.empty()) {
                a["path"] = local;
                a.erase("uri");
                a.erase("missing");
                continue;
            }
        }
        if (a.contains("generator")) continue;
        std::string p = a.value("path", std::string());
        if (p.empty() || !fileExists(p)) {
            // Never silently substitute: mark missing for the relink UI.
            a["missing"] = true;
            warnings.push_back("Missing media: " + a.value("name", std::string()) + " (relink to restore).");
        }
    }
    for (auto& [p, f] : r.extracted)
        if (p.rfind("fonts/", 0) == 0) warnings.push_back("Embedded font extracted: " + pathBasename(f));
    for (auto& [p, d] : r.pkg.files)
        if (p.rfind("fonts/", 0) == 0) {
            std::string out = pathJoin(mediaDir, pathBasename(p));
            atomicWriteFile(out, std::string(d.begin(), d.end()));
            warnings.push_back("Embedded font extracted: " + pathBasename(p));
        }
    for (auto& w : r.warnings) warnings.push_back(w);
    docOut = doc;
    return true;
}

bool exportJsonPackage(const std::string& path, const std::string& kind, const json& content, const json& extra, const PackageWriteOptions& opt,
                       std::string& err) {
    Package pkg;
    pkg.kind = kind;
    if (extra.is_object()) pkg.manifest = extra;
    if (kind == "script") {
        std::string src = content.value("source", std::string());
        pkg.files["script.js"] = std::vector<uint8_t>(src.begin(), src.end());
        json meta = content;
        meta.erase("source");
        std::string ui = meta.value("ui", json::object()).dump();
        pkg.files["ui.json"] = std::vector<uint8_t>(ui.begin(), ui.end());
        pkg.manifest["script"] = meta;
    } else {
        std::string s = content.dump();
        pkg.files["content.json"] = std::vector<uint8_t>(s.begin(), s.end());
    }
    return writePackage(path, pkg, opt, err);
}

bool importJsonPackage(const std::string& path, const std::string& expectedKind, json& content, json& manifest, const PackageReadOptions& opt,
                       std::string& err, std::vector<std::string>* warnings) {
    PackageReadResult r = readPackage(path, opt);
    if (!r.ok) { err = r.error; return false; }
    if (!expectedKind.empty() && r.pkg.kind != expectedKind) { err = "Expected a " + expectedKind + " package but this is a " + r.pkg.kind + " package."; return false; }
    manifest = r.pkg.manifest;
    manifest["_signed"] = r.signedPkg;
    manifest["_trusted"] = r.trusted;
    manifest["_signer"] = r.signer;
    if (r.pkg.kind == "script") {
        auto it = r.pkg.files.find("script.js");
        if (it == r.pkg.files.end()) { err = "Script package has no script.js."; return false; }
        content = jobj(manifest, "script");
        content["source"] = std::string(it->second.begin(), it->second.end());
    } else {
        auto it = r.pkg.files.find("content.json");
        if (it == r.pkg.files.end()) { err = "Package content missing."; return false; }
        content = json::parse(it->second.begin(), it->second.end(), nullptr, false);
        if (content.is_discarded()) { err = "Package content is damaged."; return false; }
    }
    if (warnings) *warnings = r.warnings;
    return true;
}


// ---------------------------------------------------------------- capsules
namespace {
void collectCapsuleFonts(const json& comp, std::set<std::string>& fonts) {
    for (auto& L : jarr(comp, "layers")) {
        if (L.contains("text")) fonts.insert(L["text"].value("font", std::string()));
        if (L.contains("captions")) fonts.insert(jobj(L["captions"], "style").value("font", std::string()));
    }
}
}  // namespace

bool exportCapsulePackage(const std::string& outPath, const json& capsule, const std::function<std::string(const json&)>& assetPath,
                          const std::function<std::string(const std::string&)>& fontPath, const PackageWriteOptions& opt, std::string& err,
                          std::vector<std::string>* warnings) {
    if (!capsule.contains("comp")) { err = "Capsule has no composition."; return false; }
    Package pkg;
    pkg.kind = "capsule";
    json cap = capsule;
    // Fonts used anywhere inside the capsule.
    std::set<std::string> fonts;
    collectCapsuleFonts(cap["comp"], fonts);
    for (auto& c : jarr(cap, "comps")) collectCapsuleFonts(c, fonts);
    json fontList = json::array();
    for (auto& f : fonts) {
        if (f.empty()) continue;
        std::string p = fontPath ? fontPath(f) : std::string();
        if (p.empty() || !fileExists(p)) {
            if (warnings) warnings->push_back("Font '" + f + "' could not be embedded; the receiver needs it installed.");
            continue;
        }
        std::string name = "fonts/" + f + "." + pathExtensionLower(p);
        pkg.diskFiles[name] = p;
        fontList.push_back({{"name", f}, {"file", name}});
    }
    // Media files.
    for (auto& a : cap["assets"]) {
        if (a.contains("generator")) continue;
        std::string p = assetPath ? assetPath(a) : std::string();
        if (p.empty() || !fileExists(p)) {
            if (warnings) warnings->push_back("Media '" + a.value("name", std::string()) + "' could not be embedded.");
            continue;
        }
        std::string ext = pathExtensionLower(p);
        std::string name = "media/" + a.value("id", std::string("asset")) + (ext.empty() ? "" : "." + ext);
        pkg.diskFiles[name] = p;
        a["packagePath"] = name;
        a.erase("uri");
        a.erase("path");
    }
    cap["fonts"] = fontList;
    std::string s = cap.dump();
    pkg.files["content.json"] = std::vector<uint8_t>(s.begin(), s.end());
    json ctl = json::array();
    for (auto& c : jarr(cap, "controls")) ctl.push_back({{"name", c.value("name", std::string())}, {"type", c.value("type", std::string())}});
    pkg.manifest = {{"name", cap.value("name", std::string("Capsule"))}, {"version", cap.value("version", std::string("1.0"))}, {"controls", ctl},
                    {"fonts", fontList.size()}, {"media", pkg.diskFiles.size() - fontList.size()}};
    return writePackage(outPath, pkg, opt, err);
}

bool importCapsulePackage(const std::string& path, const std::string& mediaDir, json& capsuleOut, std::vector<std::string>& fontFiles,
                          const PackageReadOptions& opt0, std::string& err, std::vector<std::string>* warnings) {
    PackageReadOptions opt = opt0;
    opt.extractDir = mediaDir;
    PackageReadResult r = readPackage(path, opt);
    if (!r.ok) { err = r.error; return false; }
    if (r.pkg.kind != "capsule") { err = "This file is not a capsule package (.mfcapsule)."; return false; }
    auto it = r.pkg.files.find("content.json");
    if (it == r.pkg.files.end()) { err = "Capsule data missing from package."; return false; }
    json cap = json::parse(std::string(it->second.begin(), it->second.end()), nullptr, false);
    if (cap.is_discarded() || !cap.contains("comp")) { err = "Capsule data is damaged."; return false; }
    makeDirs(mediaDir);
    auto extract = [&](const std::string& pp) -> std::string {
        if (r.extracted.count(pp)) return r.extracted[pp];
        auto f = r.pkg.files.find(pp);
        if (f == r.pkg.files.end()) return std::string();
        std::string local = pathJoin(mediaDir, pathBasename(pp));
        atomicWriteFile(local, std::string(f->second.begin(), f->second.end()));
        return local;
    };
    for (auto& a : cap["assets"]) {
        std::string pp = a.value("packagePath", std::string());
        if (pp.empty()) continue;
        std::string local = extract(pp);
        if (local.empty()) {
            a["missing"] = true;
            if (warnings) warnings->push_back("Media '" + a.value("name", std::string()) + "' is missing from the package.");
            continue;
        }
        a["path"] = local;
        a.erase("packagePath");
    }
    for (auto& f : jarr(cap, "fonts")) {
        std::string local = extract(f.value("file", std::string()));
        if (!local.empty()) fontFiles.push_back(local);
    }
    for (auto& w : r.warnings)
        if (warnings) warnings->push_back(w);
    capsuleOut = cap;
    return true;
}

}  // namespace mf
