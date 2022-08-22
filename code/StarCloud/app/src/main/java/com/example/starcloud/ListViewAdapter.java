package com.example.starcloud;

import android.content.Context;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.ImageView;
import android.widget.TextView;

import java.util.List;

class ListViewAdapter extends ArrayAdapter{



    private final int resourceId;

    public ListViewAdapter(Context context, int textViewResourceId, List<ItemContent> objects) {
        super(context, textViewResourceId, objects);
        resourceId = textViewResourceId;
    }
    @Override
    public View getView(int position, View convertView, ViewGroup parent) {
        ItemContent item = (ItemContent) getItem(position); // 获取当前项实例
        View view = LayoutInflater.from(getContext()).inflate(resourceId, null);//实例化一个对象


        ImageView shopImage = (ImageView) view.findViewById(R.id.item_image);//获取该布局内的图片视图
        TextView shopName = (TextView) view.findViewById(R.id.item_name);//获取该布局内的文本视图
        shopImage.setImageResource(item.getImageId());//为图片视图设置图片资源
        shopName.setText(item.getName());//为文本视图设置文本内容
        return view;
    }
}

