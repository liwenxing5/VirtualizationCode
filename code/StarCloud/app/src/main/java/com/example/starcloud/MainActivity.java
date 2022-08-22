package com.example.starcloud;

import android.Manifest;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.ListView;
import android.widget.Toast;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;

import com.example.starcloud.databinding.ActivityMainBinding;

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
import java.util.ArrayList;

public class MainActivity extends AppCompatActivity {

    int FILE_REQ_CODE = 10;
    int SUCCESS = 0;
    int EXIST = 1;
    int FAIL = 2;
    int retCode = 0;
    Boolean listFinish = false;
    Boolean downloadFinish = false;
    String WEBURL = "http://5db2289.r3.cpolar.top";
    String LISTURL = WEBURL;
    String DOWNLOAD_URL = null;
    String DOWNLOAD_FILE = null;
    String retStr = "";
    String ROOTPATH = null;
    String DOWNLOADPATH = null;
    String SYSPATH = null;
    Socket cliSocket = null;
    ListView fileListView = null;
    ArrayList<String> fileList = new ArrayList<String>();
    private ActivityMainBinding binding;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        binding = ActivityMainBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());
        ActivityCompat.requestPermissions(this, new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE}, 222);

        ROOTPATH = Environment.getExternalStorageDirectory().getAbsolutePath()+"/StarCloud/";
        SYSPATH = ROOTPATH+"sys/";
        DOWNLOADPATH = ROOTPATH+"download/";
        new File(ROOTPATH).mkdir();
        new File(SYSPATH).mkdir();
        new File(DOWNLOADPATH).mkdir();

        binding.fab.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                Uri uri = Uri.parse("content://com.android.externalstorage.documents/document/primary:%2fStarCloud%2fdownload%2f");
                Intent localIntent = new Intent(Intent.ACTION_GET_CONTENT);
                localIntent.setType("*/*");
                localIntent.putExtra(DocumentsContract.EXTRA_INITIAL_URI, uri);
                startActivityForResult(localIntent, FILE_REQ_CODE);
            }
        });

        listFinish = false;
        new httpListThread().start();
        while (listFinish == false);
        fileListView = (ListView)findViewById(R.id.FileListView);
        ArrayAdapter<String> adapter = new ArrayAdapter<>
                (MainActivity.this, android.R.layout.simple_list_item_1, fileList);
        fileListView.setAdapter(adapter);
        fileListView.setOnItemClickListener(new AdapterView.OnItemClickListener() {
            @Override
            public void onItemClick(AdapterView<?> adapterView, View view, int i, long l) {
                DOWNLOAD_FILE = adapter.getItem(i);

                if (DOWNLOAD_FILE.contains("/")){
                    listFinish = false;
                    LISTURL = WEBURL + fileList.get(0) + DOWNLOAD_FILE;
                    System.out.println("===================");
                    System.out.println(LISTURL);
                    System.out.println("===================");
                    new httpListThread().start();
                    while (listFinish == false);
                    adapter.notifyDataSetChanged();
                } else {
                    DOWNLOAD_URL = WEBURL + fileList.get(0) + DOWNLOAD_FILE;
                    retStr = "";
                    retCode = SUCCESS;
                    System.out.println("===================");
                    System.out.println(DOWNLOAD_URL);
                    System.out.println("===================");
                    httpDownloadThread dt = new httpDownloadThread();
                    dt.start();
                    downloadFinish = false;
                    while(downloadFinish == false);
                    Toast.makeText(MainActivity.this,retStr,Toast.LENGTH_SHORT).show();
                }
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

    class  httpListThread extends Thread {
        @Override
        public void run() {
            super.run();

            HttpURLConnection conn = null;
            InputStream input = null;
            String content = "";

            try {
                URL url = new URL(LISTURL);
                conn = (HttpURLConnection)url.openConnection();
                input = conn.getInputStream();
                InputStreamReader ir = new InputStreamReader(input);
                BufferedReader reader = new BufferedReader(ir);

                String line = null;
                while((line = reader.readLine()) != null) {
                    content = content + line + "\n";
                }

                fileList.clear();
                int size1 = content.indexOf("<title>Index of ");
                int size2 = content.indexOf("</title>");
                System.out.println(content.substring(size1+16, size2)+"===========\n");
                fileList.add(content.substring(size1+16, size2));

                while (true)
                {
                    size1 = content.indexOf("<a href=\"");
                    size2 = content.indexOf("</a>");
                    if (size1 < 0|| size2 < 0) {
                        break;
                    }
                    String tmp = content.substring(size1 + 9, size2);
                    content = content.substring(size2 + 4);
                    fileList.add(tmp.substring(tmp.indexOf(">") + 1));
                    System.out.println(tmp+"==\n");
                }

                for (int i = fileList.size(); i <= 0; i++) {
                    System.out.println(fileList.get(i)+"\n");
                }
                listFinish = true;
            } catch (Exception e) {
                e.printStackTrace();
            } finally {
                try {
                    input.close();
                    conn.disconnect();
                } catch (Exception e) {
                    e.printStackTrace();
                }
            }
        }
    }


    class  httpDownloadThread extends Thread {
        @Override
        public void run() {
            super.run();

            String fileName = DOWNLOADPATH + DOWNLOAD_FILE;
            OutputStream output = null;
            URL url = null;
            HttpURLConnection conn = null;
            InputStream input = null;
            File file = null;

            try {
                url = new URL(DOWNLOAD_URL);
                conn = (HttpURLConnection)url.openConnection();
                input = conn.getInputStream();
                file = new File(fileName);
                if (file.exists())  {
                    retStr = "文件已存在" + fileName;
                    retCode = EXIST;
                    downloadFinish = true;
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
                } finally {
                    downloadFinish = true;
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