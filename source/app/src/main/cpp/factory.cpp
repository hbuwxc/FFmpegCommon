//
// Created by wxc on 2020/6/8.
//
#include <jni.h>
#include "FFmpegCenter.h"

#include <cassert>

static JNINativeMethod methods[] = {
        {"transcodeWithFilter", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", (void*)transcode_with_filter},
};

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved){
    JNIEnv* env;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }

    // Find your class. JNI_OnLoad is called from the correct class loader context for this to work.
    jclass c = env->FindClass("com/watts/ffmpeg_common/FFmpegFactory");
    if (c == nullptr) return JNI_ERR;
    int rc = env->RegisterNatives(c, methods, sizeof(methods)/sizeof(JNINativeMethod));
    if (rc != JNI_OK) return rc;

    return JNI_VERSION_1_6;
}
