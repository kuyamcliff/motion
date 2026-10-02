// MediaProvider backed by Android decoders (MediaCodec / MediaMetadataRetriever / BitmapFactory) via JNI.
#pragma once
#include <jni.h>

#include <list>
#include <map>
#include <mutex>

#include "mf/renderer.hpp"

namespace mfa {

JNIEnv* env();  // JNIEnv for the current thread (attaches if needed)
void setVm(JavaVM* vm);
void copyBitmapToImage(JNIEnv* e, jobject bitmap, mf::Image& out);

class AndroidMediaProvider : public mf::MediaProvider {
   public:
    explicit AndroidMediaProvider(JNIEnv* e);
    mf::ImagePtr videoFrame(const mf::json& asset, double sourceTime, int maxW, int maxH) override;
    mf::ImagePtr image(const mf::json& asset, int maxW, int maxH) override;
    std::shared_ptr<const mf::AudioBuffer> audio(const mf::json& asset) override;
    bool available(const mf::json& asset) override;
    void clear();
    void setFrameCacheBytes(size_t b) { frameBudget_ = b; }

   private:
    jclass bridge_ = nullptr;
    jmethodID midVideo_ = nullptr, midImage_ = nullptr, midAudio_ = nullptr, midAvail_ = nullptr;
    std::mutex m_;
    mf::FileMediaProvider generator_;
    std::list<std::pair<std::string, mf::ImagePtr>> lru_;
    std::map<std::string, std::list<std::pair<std::string, mf::ImagePtr>>::iterator> index_;
    size_t bytes_ = 0, frameBudget_ = 96u << 20;
    std::map<std::string, std::shared_ptr<const mf::AudioBuffer>> audio_;
    void put(const std::string& k, mf::ImagePtr img);
    mf::ImagePtr get(const std::string& k);
};

}  // namespace mfa
