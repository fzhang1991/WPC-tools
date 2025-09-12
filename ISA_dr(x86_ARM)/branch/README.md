
# DynamoRIO 安装与配置指南

## 概述

本文档提供下载 DynamoRIO 并切换到特定提交版本（17473b01997a744c209b84a630bc2eaef4a5179e）的完整指南。

## 安装步骤

### 1. 克隆 DynamoRIO 仓库

git clone https://github.com/DynamoRIO/dynamorio.git

cd dynamorio

git checkout 17473b01997a744c209b84a630bc2eaef4a5179e

#replace dynamorio/clients/drcachesim/tools/view.cpp by the view.cpp in this folder

mkdir build

cd build

cmake ..

make -j

```bash


