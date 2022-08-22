package com.example.starcloud;

public class ItemContent {
    private String name;
    private int imageId;

    public ItemContent(String name, int imageId) {
        this.name = name;
        this.imageId = imageId;
    }

    public String getName() {
        return name;
    }

    public int getImageId() {
        return imageId;
    }
}
