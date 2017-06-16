#include "asset_manager_file_source.hpp"

#include <mbgl/storage/response.hpp>
#include <mbgl/util/util.hpp>
#include <mbgl/util/threaded_object.hpp>
#include <mbgl/util/url.hpp>

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

namespace mbgl {

class AssetManagerFileSource::Impl {
public:
    Impl(ActorRef<Impl>, AAssetManager* assetManager_) : assetManager(assetManager_) {
    }

    void request(const std::string& url, FileSource::Callback callback) {
        // Note: AssetManager already prepends "assets" to the filename.
        const std::string path = mbgl::util::percentDecode(url.substr(8));

        Response response;

        if (AAsset* asset = AAssetManager_open(assetManager, path.c_str(), AASSET_MODE_BUFFER)) {
            response.data = std::make_shared<std::string>(
                reinterpret_cast<const char*>(AAsset_getBuffer(asset)), AAsset_getLength64(asset));
            AAsset_close(asset);
        } else {
            response.error = std::make_unique<Response::Error>(Response::Error::Reason::NotFound,
                                                               "Could not read asset");
        }

        callback(response);
    }

private:
    AAssetManager* assetManager;
};

AssetManagerFileSource::AssetManagerFileSource(jni::JNIEnv& env, jni::Object<android::AssetManager> assetManager_)
    : assetManager(assetManager_.NewGlobalRef(env)),
      impl(std::make_unique<util::ThreadedObject<Impl>>("AssetManagerFileSource",
          AAssetManager_fromJava(&env, jni::Unwrap(**assetManager)))),
      thread(std::make_unique<ActorRef<Impl>>(impl->actor())) {
}

AssetManagerFileSource::~AssetManagerFileSource() = default;

std::unique_ptr<AsyncRequest> AssetManagerFileSource::request(const Resource& resource, Callback callback) {
    class AssetFileRequest : public AsyncRequest {
    public:
        AssetFileRequest(Resource resource_, FileSource::Callback callback_, ActorRef<AssetManagerFileSource::Impl> fs_)
            : mailbox(std::make_shared<Mailbox>(*util::RunLoop::Get()))
            , fs(fs_) {
            fs.invoke(&AssetManagerFileSource::Impl::request, resource_.url, [callback_, ref = ActorRef<AssetFileRequest>(*this, mailbox)](Response res) mutable {
                ref.invoke(&AssetFileRequest::runCallback, callback_, res);
            });
        }

        void runCallback(FileSource::Callback callback_, const Response& res) {
            callback_(res);
        }

    private:
        std::shared_ptr<Mailbox> mailbox;
        ActorRef<AssetManagerFileSource::Impl> fs;
    };

    return std::make_unique<AssetFileRequest>(resource, callback, *thread);
}

} // namespace mbgl
