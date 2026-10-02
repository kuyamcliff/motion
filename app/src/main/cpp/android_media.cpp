#include "android_media.h"

#include <android/bitmap.h>
#include <android/log.h>

#include "mf/audio.hpp"

namespace mfa {

static JavaVM* g_vm = nullptr;

void setVm(JavaVM* vm) { g_vm = vm; }

namespace {
struct Detacher {
    bool attached = false;
    ~Detacher() {
        if (attached && g_vm) g_vm->DetachCurrentThread();
    }
};
thread_local Detacher t_detacher;
}  // namespace

JNIEnv* env() {
    JNIEnv* e = nullptr;
    if (!g_vm) return nullptr;
    if (g_vm->GetEnv((void**)&e, JNI_VERSION_1_6) == JNI_OK) return e;
    if (g_vm->AttachCurrentThread(&e, nullptr) == JNI_OK) {
        t_detacher.attached = true;
        return e;
    }
    return nullptr;
}

void copyBitmapToImage(JNIEnv* e, jobject bitmap, mf::Image& out) {
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(e, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS || info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) return;
    void* px = nullptr;
    if (AndroidBitmap_lockPixels(e, bitmap, &px) != ANDROID_BITMAP_RESULT_SUCCESS) return;
    out.resize((int)info.width, (int)info.height);
    for (uint32_t y = 0; y < info.height; ++y) std::memcpy(out.row((int)y), (uint8_t*)px + (size_t)y * info.stride, (size_t)info.width * 4);
    AndroidBitmap_unlockPixels(e, bitmap);
    // Bitmaps from decoders may be marked non-premultiplied (opaque video is unaffected).
    if (!(info.flags & ANDROID_BITMAP_FLAGS_ALPHA_PREMUL) && (info.flags & ANDROID_BITMAP_FLAGS_ALPHA_MASK) != ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE) {
        for (size_t i = 0; i < out.px.size(); i += 4) {
            uint32_t a = out.px[i + 3];
            if (a == 255) continue;
            for (int k = 0; k < 3; ++k) out.px[i + k] = (uint8_t)((out.px[i + k] * a + 127) / 255);
        }
    }
}

AndroidMediaProvider::AndroidMediaProvider(JNIEnv* e) {
    jclass c = e->FindClass("com/motionforge/app/media/MediaBridge");
    if (!c) { e->ExceptionClear(); return; }
    bridge_ = (jclass)e->NewGlobalRef(c);
    midVideo_ = e->GetStaticMethodID(bridge_, "videoFrame", "(Ljava/lang/String;DII)Landroid/graphics/Bitmap;");
    midImage_ = e->GetStaticMethodID(bridge_, "image", "(Ljava/lang/String;II)Landroid/graphics/Bitmap;");
    midAudio_ = e->GetStaticMethodID(bridge_, "audioPcmPath", "(Ljava/lang/String;)Ljava/lang/String;");
    midAvail_ = e->GetStaticMethodID(bridge_, "available", "(Ljava/lang/String;)Z");
    if (e->ExceptionCheck()) e->ExceptionClear();
}

void AndroidMediaProvider::put(const std::string& k, mf::ImagePtr img) {
    if (!img) return;
    lru_.push_front({k, img});
    index_[k] = lru_.begin();
    bytes_ += img->px.size();
    while (bytes_ > frameBudget_ && lru_.size() > 1) {
        bytes_ -= lru_.back().second->px.size();
        index_.erase(lru_.back().first);
        lru_.pop_back();
    }
}

mf::ImagePtr AndroidMediaProvider::get(const std::string& k) {
    auto it = index_.find(k);
    if (it == index_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second);
    return it->second->second;
}

void AndroidMediaProvider::clear() {
    std::lock_guard<std::mutex> lk(m_);
    lru_.clear();
    index_.clear();
    bytes_ = 0;
    audio_.clear();
}

static jobject callBitmap(JNIEnv* e, jclass cls, jmethodID mid, const std::string& asset, double t, int w, int h, bool video) {
    jstring js = e->NewStringUTF(asset.c_str());
    jobject bmp = video ? e->CallStaticObjectMethod(cls, mid, js, (jdouble)t, (jint)w, (jint)h) : e->CallStaticObjectMethod(cls, mid, js, (jint)w, (jint)h);
    e->DeleteLocalRef(js);
    if (e->ExceptionCheck()) {
        e->ExceptionDescribe();
        e->ExceptionClear();
        return nullptr;
    }
    return bmp;
}

mf::ImagePtr AndroidMediaProvider::videoFrame(const mf::json& asset, double st, int maxW, int maxH) {
    if (asset.contains("generator")) return generator_.videoFrame(asset, st, maxW, maxH);
    double fps = std::max(1.0, asset.value("fps", 30.0));
    int frame = (int)std::floor(st * fps + 1e-6);
    // Quantize requested size to limit cache fragmentation.
    int bucket = maxW > 960 ? 0 : maxW > 480 ? 1 : 2;
    // Key on the media source too: asset ids repeat across projects and keep their id after a relink.
    std::string src = asset.value("path", std::string());
    if (src.empty()) src = asset.value("uri", std::string());
    std::string key = asset.value("id", std::string()) + "|" + src + "|" + std::to_string(frame) + "|" + std::to_string(bucket);
    {
        std::lock_guard<std::mutex> lk(m_);
        if (auto c = get(key)) return c;
    }
    JNIEnv* e = env();
    if (!e || !bridge_ || !midVideo_) return nullptr;
    int nw = asset.value("width", maxW), nh = asset.value("height", maxH);
    int rw = bucket == 0 ? nw : bucket == 1 ? std::min(nw, 960) : std::min(nw, 480);
    int rh = nw > 0 ? (int)((double)rw * nh / nw) : maxH;
    jobject bmp = callBitmap(e, bridge_, midVideo_, asset.dump(), (frame + 0.5) / fps, rw, rh, true);
    if (!bmp) return nullptr;
    auto img = std::make_shared<mf::Image>();
    copyBitmapToImage(e, bmp, *img);
    e->DeleteLocalRef(bmp);
    if (img->empty()) return nullptr;
    std::lock_guard<std::mutex> lk(m_);
    put(key, img);
    return img;
}

mf::ImagePtr AndroidMediaProvider::image(const mf::json& asset, int maxW, int maxH) {
    if (asset.contains("generator")) return generator_.image(asset, maxW, maxH);
    std::string key = "img|" + asset.value("id", std::string()) + "|" + asset.value("path", asset.value("uri", std::string()));
    {
        std::lock_guard<std::mutex> lk(m_);
        if (auto c = get(key)) return c;
    }
    JNIEnv* e = env();
    if (!e || !bridge_ || !midImage_) return nullptr;
    int nw = asset.value("width", 4096), nh = asset.value("height", 4096);
    jobject bmp = callBitmap(e, bridge_, midImage_, asset.dump(), 0, std::min(nw, 4096), std::min(nh, 4096), false);
    if (!bmp) return nullptr;
    auto img = std::make_shared<mf::Image>();
    copyBitmapToImage(e, bmp, *img);
    e->DeleteLocalRef(bmp);
    if (img->empty()) return nullptr;
    std::lock_guard<std::mutex> lk(m_);
    put(key, img);
    return img;
}

std::shared_ptr<const mf::AudioBuffer> AndroidMediaProvider::audio(const mf::json& asset) {
    if (asset.contains("toneHz")) return generator_.audio(asset);
    std::string id = asset.value("id", std::string());
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = audio_.find(id);
        if (it != audio_.end()) return it->second;
    }
    JNIEnv* e = env();
    std::shared_ptr<mf::AudioBuffer> buf;
    if (e && bridge_ && midAudio_) {
        jstring js = e->NewStringUTF(asset.dump().c_str());
        jstring path = (jstring)e->CallStaticObjectMethod(bridge_, midAudio_, js);
        e->DeleteLocalRef(js);
        if (e->ExceptionCheck()) { e->ExceptionDescribe(); e->ExceptionClear(); path = nullptr; }
        if (path) {
            const char* p = e->GetStringUTFChars(path, nullptr);
            std::string sp = p ? p : "";
            e->ReleaseStringUTFChars(path, p);
            e->DeleteLocalRef(path);
            // Cache format: raw float32 interleaved stereo @ 48 kHz.
            bool ok;
            auto bytes = mf::readFileBytes(sp, &ok);
            if (ok && !bytes.empty()) {
                buf = std::make_shared<mf::AudioBuffer>();
                buf->sampleRate = 48000;
                buf->channels = 2;
                buf->samples.resize(bytes.size() / 4);
                std::memcpy(buf->samples.data(), bytes.data(), buf->samples.size() * 4);
            }
        }
    }
    std::lock_guard<std::mutex> lk(m_);
    audio_[id] = buf;
    return buf;
}

bool AndroidMediaProvider::available(const mf::json& asset) {
    if (asset.contains("generator") || asset.contains("toneHz")) return true;
    std::string p = asset.value("path", std::string());
    if (!p.empty() && mf::fileExists(p)) return true;
    JNIEnv* e = env();
    if (!e || !bridge_ || !midAvail_) return false;
    jstring js = e->NewStringUTF(asset.dump().c_str());
    jboolean ok = e->CallStaticBooleanMethod(bridge_, midAvail_, js);
    e->DeleteLocalRef(js);
    if (e->ExceptionCheck()) { e->ExceptionClear(); return false; }
    return ok;
}

}  // namespace mfa
