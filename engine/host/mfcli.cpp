// MOTIONFORGE host CLI: run .mftest scenarios, render frames, transcribe audio, inspect packages.
#include <cstdio>
#include <cstring>

#include "mf/audio.hpp"
#include "mf/captions.hpp"
#include "mf/engine.hpp"
#include "mf/imageops.hpp"
#include "mf/model.hpp"
#include "mf/package.hpp"
#include "mf/text.hpp"

using namespace mf;

static void usage() {
    std::printf(
        "mfcli scenario <file.mftest> [workdir]     run a scripted editing scenario\n"
        "mfcli transcribe <model.bin> <audio.wav>     offline captions with whisper.cpp (prints SRT)\n"
        "mfcli render <project.json> <t> <out.png> [scale]\n"
        "mfcli package-info <file>                    validate and describe a package\n"
        "mfcli keygen                                 generate an Ed25519 signing key pair\n");
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    std::string root = MF_SOURCE_ROOT;
    FontManager::instance().registerDirectory(root + "/assets/fonts");
    std::string cmd = argv[1];
    if (cmd == "scenario" && argc >= 3) {
        bool ok;
        std::string script = readFileText(argv[2], &ok);
        if (!ok) { std::fprintf(stderr, "cannot read %s\n", argv[2]); return 2; }
        std::string work = argc > 3 ? argv[3] : pathDirname(argv[2]);
        FileMediaProvider media;
        EngineConfig cfg;
        cfg.dataDir = pathJoin(work, ".mfdata");
        cfg.cacheDir = pathJoin(work, ".mfcache");
        cfg.modelsDir = root + "/.cache/models";
        Engine engine(cfg, &media);
        ScenarioResult r = runScenario(engine, script, work);
        for (auto& l : r.log) std::printf("%s\n", l.c_str());
        if (!r.outputs.empty()) std::printf("outputs: %s\n", r.outputs.dump().c_str());
        return r.ok ? 0 : 1;
    }
    if (cmd == "transcribe" && argc >= 4) {
        AudioBuffer buf;
        std::string err;
        if (!loadWav(argv[3], buf, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
        AsrOptions opt;
        opt.modelPath = argv[2];
        opt.language = "en";
        opt.progress = [](float p) { std::fprintf(stderr, "\rprogress %3d%%", (int)(p * 100)); return true; };
        AsrResult r = transcribe(resampleMono(buf, 16000), opt);
        std::fprintf(stderr, "\n");
        if (!r.ok) { std::fprintf(stderr, "error: %s\n", r.error.c_str()); return 1; }
        std::printf("%s", toSrt(r.captions).c_str());
        std::fprintf(stderr, "language=%s audio=%.1fs processing=%.1fs words=%zu\n", r.language.c_str(), r.audioSeconds, r.processingSeconds, r.words.size());
        return 0;
    }
    if (cmd == "render" && argc >= 5) {
        bool ok;
        json doc = json::parse(readFileText(argv[2], &ok), nullptr, false);
        if (!ok || doc.is_discarded()) { std::fprintf(stderr, "bad project\n"); return 2; }
        FileMediaProvider media;
        Renderer r(&media);
        RenderSettings rs;
        rs.scale = argc > 5 ? std::atof(argv[5]) : 1.0;
        const json* c = activeComp(doc);
        Image img = r.renderFrame(doc, c ? c->value("id", "") : "", std::atof(argv[3]), rs);
        return savePng(img, argv[4]) ? 0 : 1;
    }
    if (cmd == "package-info" && argc >= 3) {
        PackageReadOptions o;
        o.developerMode = true;
        PackageReadResult r = readPackage(argv[2], o);
        std::printf("%s\n", json{{"ok", r.ok}, {"error", r.error}, {"kind", r.pkg.kind}, {"signed", r.signedPkg}, {"signer", r.signer}, {"encrypted", r.encrypted},
                                 {"warnings", r.warnings}, {"manifest", r.pkg.manifest}}.dump(2).c_str());
        return r.ok ? 0 : 1;
    }
    if (cmd == "keygen") {
        std::string sk, pk;
        generateSigningKey(sk, pk);
        std::printf("secret: %s\npublic: %s\n", sk.c_str(), pk.c_str());
        return 0;
    }
    usage();
    return 2;
}
