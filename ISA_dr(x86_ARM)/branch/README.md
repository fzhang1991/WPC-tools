
# DynamoRIO 安装与配置指南

## 概述

本文档提供使用DynamoRIO获取分支局部性的功能。

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


