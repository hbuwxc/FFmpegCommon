//
// Created by wxc on 2020/6/8.
//

#ifndef FFMPEGCOMMON_FFMPEGCENTER_H
#define FFMPEGCOMMON_FFMPEGCENTER_H


void transcode_with_filter(JNIEnv *env, jclass c, jstring input, jstring output, jstring filter);


#endif //FFMPEGCOMMON_FFMPEGCENTER_H
