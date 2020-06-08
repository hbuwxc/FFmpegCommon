package com.watts.ffmpeg_common;

/**
 * native ffmpeg function factory.
 * @author wxc on 2020/6/8.
 */
public class FFmpegFactory {
    public native static void transcodeWithFilter(String input, String output, String filter);
}
