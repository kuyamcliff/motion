// MOTIONFORGE package formats (.mforge, .mfsx, .mfext, .mffx, .mfcapsule, .mftemplate):
// zip container + manifest (version, compat, checksums) + optional Ed25519 signature + optional password encryption
// (Argon2i key derivation, XChaCha20-Poly1305 authenticated encryption via Monocypher).
#pragma once
#include <map>

#include "common.hpp"

namespace mf {

struct Package {
    std::string kind;  // project|script|extension|preset|capsule|template
    json manifest = json::object();
    std::map<std::string, std::vector<uint8_t>> files;  // path -> content (plaintext)
    std::map<std::string, std::string> diskFiles;       // path -> local file streamed at write time (large media)
};

struct PackageWriteOptions {
    std::string password;   // empty = no encryption
    std::string signingKey; // hex secret key (64 bytes) or empty
    std::string author;
};

struct PackageReadOptions {
    std::string password;
    std::vector<std::string> trustedKeys;  // hex public keys
    std::vector<std::string> revokedKeys;
    bool developerMode = false;            // allow unsigned packages that require trust
    std::string extractDir;                // if set, large entries are extracted here instead of memory
};

struct PackageReadResult {
    bool ok = false;
    std::string error;
    Package pkg;
    bool encrypted = false;
    bool signedPkg = false, signatureValid = false, trusted = false, revoked = false;
    std::string signer;  // hex public key
    std::vector<std::string> warnings;
    std::map<std::string, std::string> extracted;  // path -> local extracted file
};

std::string packageExtension(const std::string& kind);
std::string packageKindForExtension(const std::string& ext);
bool writePackage(const std::string& path, Package& pkg, const PackageWriteOptions& opt, std::string& err);
PackageReadResult readPackage(const std::string& path, const PackageReadOptions& opt);
bool isPackageEncrypted(const std::string& path);

std::string blake2bHex(const uint8_t* data, size_t n);
std::string blake2bFileHex(const std::string& path);
void generateSigningKey(std::string& secretHex, std::string& publicHex);
std::string toHex(const uint8_t* d, size_t n);
std::vector<uint8_t> fromHex(const std::string& s);
void randomBytes(uint8_t* out, size_t n);

// Project packages.
enum class ProjectPackMode { Linked, Collected, Portable };
// assetPath resolves an asset to a readable local file ("" if not available). fontPath resolves font names.
bool exportProjectPackage(const json& doc, const std::string& outPath, ProjectPackMode mode, const std::function<std::string(const json&)>& assetPath,
                          const std::function<std::string(const std::string&)>& fontPath, const PackageWriteOptions& opt, std::string& err,
                          const std::string& thumbnailPng = std::string());
// Imports a project package; collected media extracted to mediaDir and assets re-pointed. Missing media are flagged, never substituted.
bool importProjectPackage(const std::string& path, const std::string& mediaDir, json& docOut, std::vector<std::string>& warnings,
                          const PackageReadOptions& opt, std::string& err);

// Effect preset / capsule / template / script helpers (content JSON in package entry "content.json" or "script.js").
bool exportJsonPackage(const std::string& path, const std::string& kind, const json& content, const json& extraManifest,
                       const PackageWriteOptions& opt, std::string& err);
bool importJsonPackage(const std::string& path, const std::string& expectedKind, json& content, json& manifest, const PackageReadOptions& opt,
                       std::string& err, std::vector<std::string>* warnings = nullptr);

}  // namespace mf
