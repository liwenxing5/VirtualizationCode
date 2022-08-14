package com.example.starcloud;

import android.Manifest;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Bundle;

import com.google.android.material.snackbar.Snackbar;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AppCompatActivity;

import android.os.Environment;
import android.provider.DocumentsContract;
import android.util.Log;
import android.view.View;

import androidx.core.app.ActivityCompat;
import androidx.navigation.NavController;
import androidx.navigation.Navigation;
import androidx.navigation.ui.AppBarConfiguration;
import androidx.navigation.ui.NavigationUI;

import com.example.starcloud.databinding.ActivityMainBinding;

import android.view.Menu;
import android.view.MenuItem;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.RelativeLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.Socket;
import java.net.URL;
import java.nio.charset.StandardCharsets;

public class MainActivity extends AppCompatActivity {

    int FILE_REQ_CODE = 10;
    int SUCCESS = 0;
    int EXIST = 1;
    int FAIL = 2;
    int retCode = 0;
    String retStr = "";
    String rootPath = null;
    Socket cliSocket = null;
    private ActivityMainBinding binding;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        binding = ActivityMainBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());
        setSupportActionBar(binding.toolbar);
        ActivityCompat.requestPermissions(this, new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE}, 222);

        rootPath = Environment.getExternalStorageDirectory().getAbsolutePath()+"/StarCloud/";
        binding.fab.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                Uri uri = Uri.parse("content://com.android.externalstorage.documents/document/primary:%2fStarCloud%2f");
                new File(rootPath).mkdir();

                httpDownloadThread th = new httpDownloadThread();
                retStr = "";
                retCode = SUCCESS;
                th.start();

                Intent localIntent = new Intent(Intent.ACTION_GET_CONTENT);
                localIntent.setType("*/*");
                localIntent.putExtra(DocumentsContract.EXTRA_INITIAL_URI, uri);
                startActivityForResult(localIntent, FILE_REQ_CODE);
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, @Nullable Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == FILE_REQ_CODE && resultCode == RESULT_OK) {
            if (data.getData() != null) {
                try {
                    /*
                    Uri uri = data.getData();
                    String path = uri.getPath().toString();

                    TextView tView = new TextView(this);
                    tView.setTextSize(30);
                    tView.setText(path+"---"+rootPath);
                    tView.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));

                    LinearLayout layout = new LinearLayout(this);
                    layout.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,ViewGroup.LayoutParams.MATCH_PARENT));
                    layout.setOrientation(LinearLayout.VERTICAL);
                    layout.addView(tView);
                    setContentView(layout);*/

                } catch (Exception e) {

                }
            }
        }
    }

    class  httpDownloadThread extends Thread {
        @Override
        public void run() {
            super.run();

            String urlStr="http://19b0ae1c.vip.cpolar.cn/test.txt";
            String fileName = rootPath + "test.txt";
            OutputStream output = null;
            URL url = null;
            HttpURLConnection conn = null;
            InputStream input = null;
            File file = null;

            try {
                url = new URL(urlStr);
                conn = (HttpURLConnection)url.openConnection();
                input = conn.getInputStream();
                new File(rootPath).mkdir();
                file = new File(fileName);
                if (file.exists())  {
                    retStr = "文件已存在" + fileName;
                    retCode = EXIST;
                    return;
                }

                file.createNewFile();
                output = new FileOutputStream(file);
                byte [] buf = new byte[4096];
                while (input.read(buf) != -1) {
                    output.write(buf);
                }
                output.flush();

            } catch (Exception e) {
                e.printStackTrace();
                retStr = "下载";
                retCode = FAIL;
            } finally {
                try {
                    output.close();
                    input.close();
                    conn.disconnect();
                    retCode = SUCCESS;
                    retStr = "下载成功，已保存为"+fileName;
                    System.out.println("下载成功，已保存为"+fileName);
                } catch (Exception e) {
                    retCode = FAIL;
                    retStr = "关闭链接失败,文件存为："+fileName;
                    e.printStackTrace();
                }
            }
        }
    }
    class cliThread extends Thread {
        @Override
        public void run() {
            super.run();
            try {
                if (cliSocket == null) {
                    cliSocket = new Socket("g22112j462.iask.in", 10791);
                    OutputStream cliOS = cliSocket.getOutputStream();

                    if (cliSocket != null && cliSocket.isConnected()) {
                        String testStr = "test";
                        cliOS.write(testStr.getBytes(), 0, testStr.length());
                    }
                }
            } catch (Exception e) {
                e.printStackTrace();
            }
        }
    }

    class recvTheard extends Thread{
        @Override
        public void run() {
            super.run();
            try {
                while (cliSocket != null) {
                    InputStream cliIS = cliSocket.getInputStream();
                    byte [] buf = new byte[1024];
                    int readLen;

                    readLen = cliIS.read(buf);
                    if (readLen < 0) {
                        cliSocket.close();
                        cliSocket = null;
                    }

                }
            } catch (IOException e) {
                e.printStackTrace();
            }
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        switch (requestCode) {
            case 222:
                //Toast.makeText(getApplicationContext(), "已申请权限", Toast.LENGTH_SHORT).show();
            default:
                super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        }
    }

}