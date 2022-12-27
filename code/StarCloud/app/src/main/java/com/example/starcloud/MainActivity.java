package com.example.starcloud;

import static androidx.constraintlayout.motion.utils.Oscillator.TAG;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.util.Log;
import android.view.View;
import android.widget.AdapterView;
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

    int FILE_REQ_CODE = 1;
    int SUCCESS = 0;
    int EXIST = 1;
    int FAIL = 2;
    int retCode = 0;
    Boolean listFinish = false;
    Boolean downloadFinish = false;
    String WEBURL = "http://starcloud.5gzvip.91tunnel.com";
    String LISTURL = WEBURL;
    String DOWNLOAD_URL = null;
    String DOWNLOAD_FILE = null;
    String SEL_FILE_PATH = null;
    String retStr = "";
    String ROOTPATH = null;
    String DOWNLOADPATH = null;
    String SYSPATH = null;
    Socket cliSocket = null;
    ListView fileListView = null;
    ArrayList<ItemContent> itemList = new ArrayList<ItemContent>();
    ArrayList<String> fileList = new ArrayList<String>();
    private ActivityMainBinding binding;

    void updateItemList() {
        itemList.clear();
        for (int i = 0; i < fileList.size(); i++)
        {
            int icon_id;
            if (fileList.get(i).contains("/")) {
                icon_id = R.drawable.file_icon;
            } else if (fileList.get(i).contains(".doc")) {
                icon_id = R.drawable.word_icon;
            } else if (fileList.get(i).contains(".iso")) {
                icon_id = R.drawable.iso_icon;
            } else if (fileList.get(i).contains(".jpg")) {
                icon_id = R.drawable.image_icon;
            } else if (fileList.get(i).contains(".mp3")) {
                icon_id = R.drawable.music_icon;
            } else if (fileList.get(i).contains(".mp4")) {
                icon_id = R.drawable.video_icon;
            } else if (fileList.get(i).contains(".pdf")) {
                icon_id = R.drawable.pdf_icon;
            } else if (fileList.get(i).contains(".xls")) {
                icon_id = R.drawable.excel_icon;
            } else if (fileList.get(i).contains(".ppt")) {
                icon_id = R.drawable.ppt_icon;
            } else if (fileList.get(i).contains(".zip")) {
                icon_id = R.drawable.packet_icon;
            } else {
                icon_id = R.drawable.other_icon;
            }
            itemList.add(new ItemContent(fileList.get(i), icon_id));
        }
    }

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
                localIntent.addCategory(Intent.CATEGORY_OPENABLE);
                localIntent.setType("*/*");
                localIntent.putExtra(DocumentsContract.EXTRA_INITIAL_URI, uri);
                startActivityForResult(localIntent, FILE_REQ_CODE);
            }
        });

        listFinish = false;
        new httpListThread().start();
        while (listFinish == false);


        updateItemList();
        fileListView = (ListView)findViewById(R.id.FileListView);
        ListViewAdapter adapter = new ListViewAdapter(MainActivity.this, R.layout.listview_item, itemList);
        fileListView.setAdapter(adapter);
        fileListView.setOnItemClickListener(new AdapterView.OnItemClickListener() {
            @Override
            public void onItemClick(AdapterView<?> adapterView, View view, int i, long l) {
                ItemContent itemTmp = (ItemContent)adapter.getItem(i);
                DOWNLOAD_FILE = itemTmp.getName();
                if (DOWNLOAD_FILE.contains("/")){
                    listFinish = false;
                    LISTURL = WEBURL + itemList.get(0).getName() + DOWNLOAD_FILE;
                    new httpListThread().start();
                    while (listFinish == false);
                    updateItemList();
                    adapter.notifyDataSetChanged();
                } else {
                    DOWNLOAD_URL = WEBURL + itemList.get(0).getName() + DOWNLOAD_FILE;
                    retStr = "";
                    retCode = SUCCESS;
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
        if(resultCode== Activity.RESULT_OK)
        {
            /**
             * 当选择的图片不为空的话，在获取到图片的途径
             */
            Uri uri = data.getData();
            Log.e(TAG, "uri = "+ uri);

            try {
                String path = FilesUtils.getPath(getBaseContext(), uri);
                if(path.endsWith("jpg")||path.endsWith("png") ||path.endsWith("jpeg"))
                {
                    SEL_FILE_PATH = path;
                    //Bitmap bitmap = BitmapFactory.decodeStream(cr.openInputStream(uri));
                    System.out.println("=========" + SEL_FILE_PATH);
                    new httpUploadThread().start();
                    //imageView.setImageBitmap(bitmap);
                }
            } catch (Exception e) {
                System.out.println("====NO=====" );
                e.printStackTrace();
            }
        }

        super.onActivityResult(requestCode, resultCode, data);
    }

    class httpListThread extends Thread {
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

    class httpUploadThread extends Thread {
        @Override
        public void run() {
            super.run();
            File file = new File(SEL_FILE_PATH);
            String request = UploadUtil.uploadFile(file, WEBURL+"/");
            System.out.println("===" + request + SEL_FILE_PATH);
        }
    }

    class httpDownloadThread extends Thread {
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