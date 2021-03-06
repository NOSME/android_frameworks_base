/*
 * Copyright 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "TvInputHal"

//#define LOG_NDEBUG 0

#include "android_os_MessageQueue.h"
#include "android_runtime/AndroidRuntime.h"
#include "android_runtime/android_view_Surface.h"
#include <nativehelper/JNIHelp.h>
#include "jni.h"

#include <android/hardware/tv/input/1.0/ITvInputCallback.h>
#include <android/hardware/tv/input/1.0/ITvInput.h>
#include <android/hardware/tv/input/1.0/types.h>
#include <gui/Surface.h>
#include <utils/Errors.h>
#include <utils/KeyedVector.h>
#include <utils/Log.h>
#include <utils/Looper.h>
#include <utils/NativeHandle.h>
#include <hardware/tv_input.h>

using ::android::hardware::audio::common::V2_0::AudioDevice;
using ::android::hardware::tv::input::V1_0::ITvInput;
using ::android::hardware::tv::input::V1_0::ITvInputCallback;
using ::android::hardware::tv::input::V1_0::Result;
using ::android::hardware::tv::input::V1_0::TvInputDeviceInfo;
using ::android::hardware::tv::input::V1_0::TvInputEvent;
using ::android::hardware::tv::input::V1_0::TvInputEventType;
using ::android::hardware::tv::input::V1_0::TvInputType;
using ::android::hardware::tv::input::V1_0::TvStreamConfig;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::hardware::hidl_vec;
using ::android::hardware::hidl_string;

namespace android {

static struct {
    jmethodID deviceAvailable;
    jmethodID deviceUnavailable;
    jmethodID streamConfigsChanged;
    jmethodID firstFrameCaptured;
} gTvInputHalClassInfo;

static struct {
    jclass clazz;
} gTvStreamConfigClassInfo;

static struct {
    jclass clazz;

    jmethodID constructor;
    jmethodID streamId;
    jmethodID type;
    jmethodID maxWidth;
    jmethodID maxHeight;
    jmethodID generation;
    jmethodID build;
} gTvStreamConfigBuilderClassInfo;

static struct {
    jclass clazz;

    jmethodID constructor;
    jmethodID deviceId;
    jmethodID type;
    jmethodID hdmiPortId;
    jmethodID cableConnectionStatus;
    jmethodID audioType;
    jmethodID audioAddress;
    jmethodID build;
} gTvInputHardwareInfoBuilderClassInfo;

////////////////////////////////////////////////////////////////////////////////

class BufferProducerThread : public Thread {
public:
    BufferProducerThread(tv_input_device_t* device, int deviceId, const tv_stream_t* stream);

    virtual status_t readyToRun();

    void setSurface(const sp<Surface>& surface);
    void onCaptured(uint32_t seq, bool succeeded);
    void shutdown();

private:
    Mutex mLock;
    Condition mCondition;
    sp<Surface> mSurface;
    tv_input_device_t* mDevice;
    int mDeviceId;
    tv_stream_t mStream;
    sp<ANativeWindowBuffer_t> mBuffer;
    enum {
        CAPTURING,
        CAPTURED,
        RELEASED,
    } mBufferState;
    uint32_t mSeq;
    bool mShutdown;

    virtual bool threadLoop();

    void setSurfaceLocked(const sp<Surface>& surface);
};

BufferProducerThread::BufferProducerThread(
        tv_input_device_t* device, int deviceId, const tv_stream_t* stream)
    : Thread(false),
      mDevice(device),
      mDeviceId(deviceId),
      mBuffer(NULL),
      mBufferState(RELEASED),
      mSeq(0u),
      mShutdown(false) {
    memcpy(&mStream, stream, sizeof(mStream));
}

status_t BufferProducerThread::readyToRun() {
    sp<ANativeWindow> anw(mSurface);
    status_t err = native_window_set_usage(anw.get(), mStream.buffer_producer.usage);
    if (err != NO_ERROR) {
        return err;
    }
    err = native_window_set_buffers_dimensions(
            anw.get(), mStream.buffer_producer.width, mStream.buffer_producer.height);
    if (err != NO_ERROR) {
        return err;
    }
    err = native_window_set_buffers_format(anw.get(), mStream.buffer_producer.format);
    if (err != NO_ERROR) {
        return err;
    }
    return NO_ERROR;
}

void BufferProducerThread::setSurface(const sp<Surface>& surface) {
    Mutex::Autolock autoLock(&mLock);
    setSurfaceLocked(surface);
}

void BufferProducerThread::setSurfaceLocked(const sp<Surface>& surface) {
    if (surface == mSurface) {
        return;
    }

    if (mBufferState == CAPTURING) {
        mDevice->cancel_capture(mDevice, mDeviceId, mStream.stream_id, mSeq);
    }
    while (mBufferState == CAPTURING) {
        status_t err = mCondition.waitRelative(mLock, s2ns(1));
        if (err != NO_ERROR) {
            ALOGE("error %d while wating for buffer state to change.", err);
            break;
        }
    }
    mBuffer.clear();
    mBufferState = RELEASED;

    mSurface = surface;
    mCondition.broadcast();
}

void BufferProducerThread::onCaptured(uint32_t seq, bool succeeded) {
    Mutex::Autolock autoLock(&mLock);
    if (seq != mSeq) {
        ALOGW("Incorrect sequence value: expected %u actual %u", mSeq, seq);
    }
    if (mBufferState != CAPTURING) {
        ALOGW("mBufferState != CAPTURING : instead %d", mBufferState);
    }
    if (succeeded) {
        mBufferState = CAPTURED;
    } else {
        mBuffer.clear();
        mBufferState = RELEASED;
    }
    mCondition.broadcast();
}

void BufferProducerThread::shutdown() {
    Mutex::Autolock autoLock(&mLock);
    mShutdown = true;
    setSurfaceLocked(NULL);
    requestExitAndWait();
}

bool BufferProducerThread::threadLoop() {
    Mutex::Autolock autoLock(&mLock);

    status_t err = NO_ERROR;
    if (mSurface == NULL) {
        err = mCondition.waitRelative(mLock, s2ns(1));
        // It's OK to time out here.
        if (err != NO_ERROR && err != TIMED_OUT) {
            ALOGE("error %d while wating for non-null surface to be set", err);
            return false;
        }
        return true;
    }
    sp<ANativeWindow> anw(mSurface);
    while (mBufferState == CAPTURING) {
        err = mCondition.waitRelative(mLock, s2ns(1));
        if (err != NO_ERROR) {
            ALOGE("error %d while wating for buffer state to change.", err);
            return false;
        }
    }
    if (mBufferState == CAPTURED && anw != NULL) {
        err = anw->queueBuffer(anw.get(), mBuffer.get(), -1);
        if (err != NO_ERROR) {
            ALOGE("error %d while queueing buffer to surface", err);
            return false;
        }
        mBuffer.clear();
        mBufferState = RELEASED;
    }
    if (mBuffer == NULL && !mShutdown && anw != NULL) {
        ANativeWindowBuffer_t* buffer = NULL;
        err = native_window_dequeue_buffer_and_wait(anw.get(), &buffer);
        if (err != NO_ERROR) {
            ALOGE("error %d while dequeueing buffer to surface", err);
            return false;
        }
        mBuffer = buffer;
        mBufferState = CAPTURING;
        mDevice->request_capture(mDevice, mDeviceId, mStream.stream_id,
                                 buffer->handle, ++mSeq);
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////

class JTvInputHal {
public:
    ~JTvInputHal();

    static JTvInputHal* createInstance(JNIEnv* env, jobject thiz, const sp<Looper>& looper);

    int addOrUpdateStream(int deviceId, int streamId, const sp<Surface>& surface);
    int removeStream(int deviceId, int streamId);
    const hidl_vec<TvStreamConfig> getStreamConfigs(int deviceId);

    void onDeviceAvailable(const TvInputDeviceInfo& info);
    void onDeviceUnavailable(int deviceId);
    void onStreamConfigurationsChanged(int deviceId, int cableConnectionStatus);
    void onCaptured(int deviceId, int streamId, uint32_t seq, bool succeeded);

private:
    // Connection between a surface and a stream.
    class Connection {
    public:
        Connection() {}

        sp<Surface> mSurface;
        tv_stream_type_t mStreamType;

        // Only valid when mStreamType == TV_STREAM_TYPE_INDEPENDENT_VIDEO_SOURCE
        sp<NativeHandle> mSourceHandle;
        // Only valid when mStreamType == TV_STREAM_TYPE_BUFFER_PRODUCER
        sp<BufferProducerThread> mThread;
    };

    class NotifyHandler : public MessageHandler {
    public:
        NotifyHandler(JTvInputHal* hal, const TvInputEvent& event);

        virtual void handleMessage(const Message& message);

    private:
        TvInputEvent mEvent;
        JTvInputHal* mHal;
    };

    class TvInputCallback : public ITvInputCallback {
    public:
        explicit TvInputCallback(JTvInputHal* hal);
        Return<void> notify(const TvInputEvent& event) override;
    private:
        JTvInputHal* mHal;
    };

    JTvInputHal(JNIEnv* env, jobject thiz, sp<ITvInput> tvInput, const sp<Looper>& looper);

    Mutex mLock;
    Mutex mStreamLock;
    jweak mThiz;
    sp<Looper> mLooper;

    KeyedVector<int, KeyedVector<int, Connection> > mConnections;

    sp<ITvInput> mTvInput;
    sp<ITvInputCallback> mTvInputCallback;
};

JTvInputHal::JTvInputHal(JNIEnv* env, jobject thiz, sp<ITvInput> tvInput,
        const sp<Looper>& looper) {
    mThiz = env->NewWeakGlobalRef(thiz);
    mTvInput = tvInput;
    mLooper = looper;
    mTvInputCallback = new TvInputCallback(this);
    mTvInput->setCallback(mTvInputCallback);
}

JTvInputHal::~JTvInputHal() {
    mTvInput->setCallback(nullptr);
    JNIEnv* env = AndroidRuntime::getJNIEnv();
    env->DeleteWeakGlobalRef(mThiz);
    mThiz = NULL;
}

JTvInputHal* JTvInputHal::createInstance(JNIEnv* env, jobject thiz, const sp<Looper>& looper) {
    // TODO(b/31632518)
    sp<ITvInput> tvInput = ITvInput::getService();
    if (tvInput == nullptr) {
        ALOGE("Couldn't get tv.input service.");
        return nullptr;
    }

    return new JTvInputHal(env, thiz, tvInput, looper);
}

int JTvInputHal::addOrUpdateStream(int deviceId, int streamId, const sp<Surface>& surface) {
    Mutex::Autolock autoLock(&mStreamLock);
    KeyedVector<int, Connection>& connections = mConnections.editValueFor(deviceId);
    if (connections.indexOfKey(streamId) < 0) {
        connections.add(streamId, Connection());
    }
    Connection& connection = connections.editValueFor(streamId);
    if (connection.mSurface == surface) {
        // Nothing to do
        return NO_ERROR;
    }
    // Clear the surface in the connection.
    if (connection.mSurface != NULL) {
        if (connection.mStreamType == TV_STREAM_TYPE_INDEPENDENT_VIDEO_SOURCE) {
            if (Surface::isValid(connection.mSurface)) {
                connection.mSurface->setSidebandStream(NULL);
            }
        }
        connection.mSurface.clear();
    }
    if (connection.mSourceHandle == NULL && connection.mThread == NULL) {
        // Need to configure stream
        Result result = Result::UNKNOWN;
        hidl_vec<TvStreamConfig> list;
        mTvInput->getStreamConfigurations(deviceId,
                [&result, &list](Result res, hidl_vec<TvStreamConfig> configs) {
                    result = res;
                    if (res == Result::OK) {
                        list = configs;
                    }
                });
        if (result != Result::OK) {
            ALOGE("Couldn't get stream configs for device id:%d result:%d", deviceId, result);
            return UNKNOWN_ERROR;
        }
        int configIndex = -1;
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i].streamId == streamId) {
                configIndex = i;
                break;
            }
        }
        if (configIndex == -1) {
            ALOGE("Cannot find a config with given stream ID: %d", streamId);
            return BAD_VALUE;
        }
        connection.mStreamType = TV_STREAM_TYPE_INDEPENDENT_VIDEO_SOURCE;

        result = Result::UNKNOWN;
        const native_handle_t* sidebandStream;
        mTvInput->openStream(deviceId, streamId,
                [&result, &sidebandStream](Result res, const native_handle_t* handle) {
                    result = res;
                    if (res == Result::OK) {
                        if (handle) {
                            sidebandStream = native_handle_clone(handle);
                        } else {
                            result = Result::UNKNOWN;
                        }
                    }
                });
        if (result != Result::OK) {
            ALOGE("Couldn't open stream. device id:%d stream id:%d result:%d", deviceId, streamId,
                    result);
            return UNKNOWN_ERROR;
        }
        connection.mSourceHandle = NativeHandle::create((native_handle_t*)sidebandStream, true);
    }
    connection.mSurface = surface;
    if (connection.mSurface != nullptr) {
        connection.mSurface->setSidebandStream(connection.mSourceHandle);
    }
    return NO_ERROR;
}

int JTvInputHal::removeStream(int deviceId, int streamId) {
    Mutex::Autolock autoLock(&mStreamLock);
    KeyedVector<int, Connection>& connections = mConnections.editValueFor(deviceId);
    if (connections.indexOfKey(streamId) < 0) {
        return BAD_VALUE;
    }
    Connection& connection = connections.editValueFor(streamId);
    if (connection.mSurface == NULL) {
        // Nothing to do
        return NO_ERROR;
    }
    if (Surface::isValid(connection.mSurface)) {
        connection.mSurface->setSidebandStream(NULL);
    }
    connection.mSurface.clear();
    if (connection.mThread != NULL) {
        connection.mThread->shutdown();
        connection.mThread.clear();
    }
    if (mTvInput->closeStream(deviceId, streamId) != Result::OK) {
        ALOGE("Couldn't close stream. device id:%d stream id:%d", deviceId, streamId);
        return BAD_VALUE;
    }
    if (connection.mSourceHandle != NULL) {
        connection.mSourceHandle.clear();
    }
    return NO_ERROR;
}

const hidl_vec<TvStreamConfig> JTvInputHal::getStreamConfigs(int deviceId) {
    Result result = Result::UNKNOWN;
    hidl_vec<TvStreamConfig> list;
    mTvInput->getStreamConfigurations(deviceId,
            [&result, &list](Result res, hidl_vec<TvStreamConfig> configs) {
                result = res;
                if (res == Result::OK) {
                    list = configs;
                }
            });
    if (result != Result::OK) {
        ALOGE("Couldn't get stream configs for device id:%d result:%d", deviceId, result);
    }
    return list;
}

void JTvInputHal::onDeviceAvailable(const TvInputDeviceInfo& info) {
    {
        Mutex::Autolock autoLock(&mLock);
        mConnections.add(info.deviceId, KeyedVector<int, Connection>());
    }
    JNIEnv* env = AndroidRuntime::getJNIEnv();

    jobject builder = env->NewObject(
            gTvInputHardwareInfoBuilderClassInfo.clazz,
            gTvInputHardwareInfoBuilderClassInfo.constructor);
    env->CallObjectMethod(
            builder, gTvInputHardwareInfoBuilderClassInfo.deviceId, info.deviceId);
    env->CallObjectMethod(
            builder, gTvInputHardwareInfoBuilderClassInfo.type, info.type);
    if (info.type == TvInputType::HDMI) {
        env->CallObjectMethod(
                builder, gTvInputHardwareInfoBuilderClassInfo.hdmiPortId, info.portId);
    }
    env->CallObjectMethod(
            builder, gTvInputHardwareInfoBuilderClassInfo.cableConnectionStatus,
            info.cableConnectionStatus);
    env->CallObjectMethod(
            builder, gTvInputHardwareInfoBuilderClassInfo.audioType, info.audioType);
    if (info.audioType != AudioDevice::NONE) {
        uint8_t buffer[info.audioAddress.size() + 1];
        memcpy(buffer, info.audioAddress.data(), info.audioAddress.size());
        buffer[info.audioAddress.size()] = '\0';
        jstring audioAddress = env->NewStringUTF(reinterpret_cast<const char *>(buffer));
        env->CallObjectMethod(
                builder, gTvInputHardwareInfoBuilderClassInfo.audioAddress, audioAddress);
        env->DeleteLocalRef(audioAddress);
    }

    jobject infoObject = env->CallObjectMethod(builder, gTvInputHardwareInfoBuilderClassInfo.build);

    env->CallVoidMethod(
            mThiz,
            gTvInputHalClassInfo.deviceAvailable,
            infoObject);

    env->DeleteLocalRef(builder);
    env->DeleteLocalRef(infoObject);
}

void JTvInputHal::onDeviceUnavailable(int deviceId) {
    {
        Mutex::Autolock autoLock(&mLock);
        KeyedVector<int, Connection>& connections = mConnections.editValueFor(deviceId);
        for (size_t i = 0; i < connections.size(); ++i) {
            removeStream(deviceId, connections.keyAt(i));
        }
        connections.clear();
        mConnections.removeItem(deviceId);
    }
    JNIEnv* env = AndroidRuntime::getJNIEnv();
    env->CallVoidMethod(
            mThiz,
            gTvInputHalClassInfo.deviceUnavailable,
            deviceId);
}

void JTvInputHal::onStreamConfigurationsChanged(int deviceId, int cableConnectionStatus) {
    {
        Mutex::Autolock autoLock(&mLock);
        KeyedVector<int, Connection>& connections = mConnections.editValueFor(deviceId);
        for (size_t i = 0; i < connections.size(); ++i) {
            removeStream(deviceId, connections.keyAt(i));
        }
        connections.clear();
    }
    JNIEnv* env = AndroidRuntime::getJNIEnv();
    env->CallVoidMethod(mThiz, gTvInputHalClassInfo.streamConfigsChanged, deviceId,
                        cableConnectionStatus);
}

void JTvInputHal::onCaptured(int deviceId, int streamId, uint32_t seq, bool succeeded) {
    sp<BufferProducerThread> thread;
    {
        Mutex::Autolock autoLock(&mLock);
        KeyedVector<int, Connection>& connections = mConnections.editValueFor(deviceId);
        Connection& connection = connections.editValueFor(streamId);
        if (connection.mThread == NULL) {
            ALOGE("capture thread not existing.");
            return;
        }
        thread = connection.mThread;
    }
    thread->onCaptured(seq, succeeded);
    if (seq == 0) {
        JNIEnv* env = AndroidRuntime::getJNIEnv();
        env->CallVoidMethod(
                mThiz,
                gTvInputHalClassInfo.firstFrameCaptured,
                deviceId,
                streamId);
    }
}

JTvInputHal::NotifyHandler::NotifyHandler(JTvInputHal* hal, const TvInputEvent& event) {
    mHal = hal;
    mEvent = event;
}

void JTvInputHal::NotifyHandler::handleMessage(const Message& message) {
    switch (mEvent.type) {
        case TvInputEventType::DEVICE_AVAILABLE: {
            mHal->onDeviceAvailable(mEvent.deviceInfo);
        } break;
        case TvInputEventType::DEVICE_UNAVAILABLE: {
            mHal->onDeviceUnavailable(mEvent.deviceInfo.deviceId);
        } break;
        case TvInputEventType::STREAM_CONFIGURATIONS_CHANGED: {
            int cableConnectionStatus = static_cast<int>(mEvent.deviceInfo.cableConnectionStatus);
            mHal->onStreamConfigurationsChanged(mEvent.deviceInfo.deviceId, cableConnectionStatus);
        } break;
        default:
            ALOGE("Unrecognizable event");
    }
}

JTvInputHal::TvInputCallback::TvInputCallback(JTvInputHal* hal) {
    mHal = hal;
}

Return<void> JTvInputHal::TvInputCallback::notify(const TvInputEvent& event) {
    mHal->mLooper->sendMessage(new NotifyHandler(mHal, event), static_cast<int>(event.type));
    return Void();
}

////////////////////////////////////////////////////////////////////////////////

static jlong nativeOpen(JNIEnv* env, jobject thiz, jobject messageQueueObj) {
    sp<MessageQueue> messageQueue =
            android_os_MessageQueue_getMessageQueue(env, messageQueueObj);
    return (jlong)JTvInputHal::createInstance(env, thiz, messageQueue->getLooper());
}

static int nativeAddOrUpdateStream(JNIEnv* env, jclass clazz,
        jlong ptr, jint deviceId, jint streamId, jobject jsurface) {
    JTvInputHal* tvInputHal = (JTvInputHal*)ptr;
    if (!jsurface) {
        return BAD_VALUE;
    }
    sp<Surface> surface(android_view_Surface_getSurface(env, jsurface));
    if (!Surface::isValid(surface)) {
        return BAD_VALUE;
    }
    return tvInputHal->addOrUpdateStream(deviceId, streamId, surface);
}

static int nativeRemoveStream(JNIEnv* env, jclass clazz,
        jlong ptr, jint deviceId, jint streamId) {
    JTvInputHal* tvInputHal = (JTvInputHal*)ptr;
    return tvInputHal->removeStream(deviceId, streamId);
}

static jobjectArray nativeGetStreamConfigs(JNIEnv* env, jclass clazz,
        jlong ptr, jint deviceId, jint generation) {
    JTvInputHal* tvInputHal = (JTvInputHal*)ptr;
    const hidl_vec<TvStreamConfig> configs = tvInputHal->getStreamConfigs(deviceId);

    jobjectArray result = env->NewObjectArray(configs.size(), gTvStreamConfigClassInfo.clazz, NULL);
    for (size_t i = 0; i < configs.size(); ++i) {
        jobject builder = env->NewObject(
                gTvStreamConfigBuilderClassInfo.clazz,
                gTvStreamConfigBuilderClassInfo.constructor);
        env->CallObjectMethod(
                builder, gTvStreamConfigBuilderClassInfo.streamId, configs[i].streamId);
        env->CallObjectMethod(
                builder, gTvStreamConfigBuilderClassInfo.type,
                        TV_STREAM_TYPE_INDEPENDENT_VIDEO_SOURCE);
        env->CallObjectMethod(
                builder, gTvStreamConfigBuilderClassInfo.maxWidth, configs[i].maxVideoWidth);
        env->CallObjectMethod(
                builder, gTvStreamConfigBuilderClassInfo.maxHeight, configs[i].maxVideoHeight);
        env->CallObjectMethod(
                builder, gTvStreamConfigBuilderClassInfo.generation, generation);

        jobject config = env->CallObjectMethod(builder, gTvStreamConfigBuilderClassInfo.build);

        env->SetObjectArrayElement(result, i, config);

        env->DeleteLocalRef(config);
        env->DeleteLocalRef(builder);
    }
    return result;
}

static void nativeClose(JNIEnv* env, jclass clazz, jlong ptr) {
    JTvInputHal* tvInputHal = (JTvInputHal*)ptr;
    delete tvInputHal;
}

static const JNINativeMethod gTvInputHalMethods[] = {
    /* name, signature, funcPtr */
    { "nativeOpen", "(Landroid/os/MessageQueue;)J",
            (void*) nativeOpen },
    { "nativeAddOrUpdateStream", "(JIILandroid/view/Surface;)I",
            (void*) nativeAddOrUpdateStream },
    { "nativeRemoveStream", "(JII)I",
            (void*) nativeRemoveStream },
    { "nativeGetStreamConfigs", "(JII)[Landroid/media/tv/TvStreamConfig;",
            (void*) nativeGetStreamConfigs },
    { "nativeClose", "(J)V",
            (void*) nativeClose },
};

#define FIND_CLASS(var, className) \
        var = env->FindClass(className); \
        LOG_FATAL_IF(! (var), "Unable to find class " className)

#define GET_METHOD_ID(var, clazz, methodName, fieldDescriptor) \
        var = env->GetMethodID(clazz, methodName, fieldDescriptor); \
        LOG_FATAL_IF(! (var), "Unable to find method" methodName)

int register_android_server_tv_TvInputHal(JNIEnv* env) {
    int res = jniRegisterNativeMethods(env, "com/android/server/tv/TvInputHal",
            gTvInputHalMethods, NELEM(gTvInputHalMethods));
    LOG_FATAL_IF(res < 0, "Unable to register native methods.");
    (void)res; // Don't complain about unused variable in the LOG_NDEBUG case

    jclass clazz;
    FIND_CLASS(clazz, "com/android/server/tv/TvInputHal");

    GET_METHOD_ID(
            gTvInputHalClassInfo.deviceAvailable, clazz,
            "deviceAvailableFromNative", "(Landroid/media/tv/TvInputHardwareInfo;)V");
    GET_METHOD_ID(
            gTvInputHalClassInfo.deviceUnavailable, clazz, "deviceUnavailableFromNative", "(I)V");
    GET_METHOD_ID(gTvInputHalClassInfo.streamConfigsChanged, clazz,
                  "streamConfigsChangedFromNative", "(II)V");
    GET_METHOD_ID(
            gTvInputHalClassInfo.firstFrameCaptured, clazz,
            "firstFrameCapturedFromNative", "(II)V");

    FIND_CLASS(gTvStreamConfigClassInfo.clazz, "android/media/tv/TvStreamConfig");
    gTvStreamConfigClassInfo.clazz = jclass(env->NewGlobalRef(gTvStreamConfigClassInfo.clazz));

    FIND_CLASS(gTvStreamConfigBuilderClassInfo.clazz, "android/media/tv/TvStreamConfig$Builder");
    gTvStreamConfigBuilderClassInfo.clazz =
            jclass(env->NewGlobalRef(gTvStreamConfigBuilderClassInfo.clazz));

    GET_METHOD_ID(
            gTvStreamConfigBuilderClassInfo.constructor,
            gTvStreamConfigBuilderClassInfo.clazz,
            "<init>", "()V");
    GET_METHOD_ID(
            gTvStreamConfigBuilderClassInfo.streamId,
            gTvStreamConfigBuilderClassInfo.clazz,
            "streamId", "(I)Landroid/media/tv/TvStreamConfig$Builder;");
    GET_METHOD_ID(
            gTvStreamConfigBuilderClassInfo.type,
            gTvStreamConfigBuilderClassInfo.clazz,
            "type", "(I)Landroid/media/tv/TvStreamConfig$Builder;");
    GET_METHOD_ID(
            gTvStreamConfigBuilderClassInfo.maxWidth,
            gTvStreamConfigBuilderClassInfo.clazz,
            "maxWidth", "(I)Landroid/media/tv/TvStreamConfig$Builder;");
    GET_METHOD_ID(
            gTvStreamConfigBuilderClassInfo.maxHeight,
            gTvStreamConfigBuilderClassInfo.clazz,
            "maxHeight", "(I)Landroid/media/tv/TvStreamConfig$Builder;");
    GET_METHOD_ID(
            gTvStreamConfigBuilderClassInfo.generation,
            gTvStreamConfigBuilderClassInfo.clazz,
            "generation", "(I)Landroid/media/tv/TvStreamConfig$Builder;");
    GET_METHOD_ID(
            gTvStreamConfigBuilderClassInfo.build,
            gTvStreamConfigBuilderClassInfo.clazz,
            "build", "()Landroid/media/tv/TvStreamConfig;");

    FIND_CLASS(gTvInputHardwareInfoBuilderClassInfo.clazz,
            "android/media/tv/TvInputHardwareInfo$Builder");
    gTvInputHardwareInfoBuilderClassInfo.clazz =
            jclass(env->NewGlobalRef(gTvInputHardwareInfoBuilderClassInfo.clazz));

    GET_METHOD_ID(
            gTvInputHardwareInfoBuilderClassInfo.constructor,
            gTvInputHardwareInfoBuilderClassInfo.clazz,
            "<init>", "()V");
    GET_METHOD_ID(
            gTvInputHardwareInfoBuilderClassInfo.deviceId,
            gTvInputHardwareInfoBuilderClassInfo.clazz,
            "deviceId", "(I)Landroid/media/tv/TvInputHardwareInfo$Builder;");
    GET_METHOD_ID(
            gTvInputHardwareInfoBuilderClassInfo.type,
            gTvInputHardwareInfoBuilderClassInfo.clazz,
            "type", "(I)Landroid/media/tv/TvInputHardwareInfo$Builder;");
    GET_METHOD_ID(
            gTvInputHardwareInfoBuilderClassInfo.hdmiPortId,
            gTvInputHardwareInfoBuilderClassInfo.clazz,
            "hdmiPortId", "(I)Landroid/media/tv/TvInputHardwareInfo$Builder;");
    GET_METHOD_ID(
            gTvInputHardwareInfoBuilderClassInfo.cableConnectionStatus,
            gTvInputHardwareInfoBuilderClassInfo.clazz,
            "cableConnectionStatus", "(I)Landroid/media/tv/TvInputHardwareInfo$Builder;");
    GET_METHOD_ID(
            gTvInputHardwareInfoBuilderClassInfo.audioType,
            gTvInputHardwareInfoBuilderClassInfo.clazz,
            "audioType", "(I)Landroid/media/tv/TvInputHardwareInfo$Builder;");
    GET_METHOD_ID(
            gTvInputHardwareInfoBuilderClassInfo.audioAddress,
            gTvInputHardwareInfoBuilderClassInfo.clazz,
            "audioAddress", "(Ljava/lang/String;)Landroid/media/tv/TvInputHardwareInfo$Builder;");
    GET_METHOD_ID(
            gTvInputHardwareInfoBuilderClassInfo.build,
            gTvInputHardwareInfoBuilderClassInfo.clazz,
            "build", "()Landroid/media/tv/TvInputHardwareInfo;");

    return 0;
}

} /* namespace android */
