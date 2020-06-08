package com.watts.ffmpeg_common;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.util.Log;
import android.view.View;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

public class MainActivity extends AppCompatActivity {

    // Used to load the 'native-lib' library on application startup.
    static {
        System.loadLibrary("avcodec");
        System.loadLibrary("avfilter");
        System.loadLibrary("postproc");
        System.loadLibrary("avutil");
        System.loadLibrary("swresample");
        System.loadLibrary("swscale");
        System.loadLibrary("function");
    }

    private String inputPath,outputPath,filterImagePath;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            if (checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(new String[]{Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE}, 0);
            } else {
                copyResource();
            }
        }
    }

    private void copyResource() {
        inputPath = Environment.getExternalStorageDirectory().getAbsolutePath() + File.separator + "/111/input.mp4";
        outputPath = Environment.getExternalStorageDirectory().getAbsolutePath() + File.separator + "/111/output.mp4";
        filterImagePath = Environment.getExternalStorageDirectory().getAbsolutePath() + File.separator + "/111/filter.png";

        File f = new File(inputPath);
        if (!f.exists()) {
            copyAssets(this, "input.mp4", inputPath);
        }

        File imageFile = new File(filterImagePath);
        if (!imageFile.exists()) {
            copyAssets(this, "filter.png", filterImagePath);
        }
    }

    public void transcodeWithFilter(View view) {
        new Thread(new Runnable() {
            @Override
            public void run() {
                createNewFile(outputPath);
                FFmpegFactory.transcodeWithFilter(inputPath,outputPath,generateFilterGraph(100));
            }
        }).start();
    }

    private String generateFilterGraph(int location){
        return "movie="+filterImagePath+"[wm];[in][wm]overlay=5:"+location+"[out]";
    }


    /**
     * create new file, delete if exist.
     * @param path
     */
    private void createNewFile(String path){
        File filterFile = new File(path);
        if (filterFile.exists()){
            filterFile.delete();
            try {
                filterFile.createNewFile();
            } catch (IOException e) {
                e.printStackTrace();
            }
        } else {
            try {
                filterFile.createNewFile();
            } catch (IOException e) {
                e.printStackTrace();
            }
        }
    }

    /**
     * copy assets to sdcard.
     * @param context
     * @param fileName
     * @param newPath
     */
    public static void copyAssets(Context context, String fileName, String newPath) {
        try {
            InputStream is = context.getAssets().open(fileName);
            File f = new File(newPath);
            if (!f.getParentFile().exists()) {
                f.getParentFile().mkdirs();
            } else if (!f.exists()) {
                f.createNewFile();
            }
            FileOutputStream fos = new FileOutputStream(f);
            byte[] buffer = new byte[1024];
            int byteCount;
            while ((byteCount = is.read(buffer)) != -1) {
                // buffer字节
                fos.write(buffer, 0, byteCount);
            }
            fos.flush();// 刷新缓冲区
            is.close();
            fos.close();
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            if (checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED) {
                copyResource();
            }
        }
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
    }
}
