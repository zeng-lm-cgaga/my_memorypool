# my_memorypool

# 内存池测试（单次测试，误差较大）

### 主机配置： 
 - 内存：4G 
 - 处理器：4核
 - 存储：60g
 - swap分区：4g

## 小对象高并发测试
<img width="714" height="627" alt="image" src="https://github.com/user-attachments/assets/3268a62f-e183-42c7-a2fa-3360ca56d5de" />

### 结论
 - 较系统调用函数快了 144%

## 中对象高并发测试
<img width="715" height="633" alt="image" src="https://github.com/user-attachments/assets/fa92a9ab-89f6-4434-b2a3-97a24313c223" />

### 结论
  - 较系统调用快了 42%

## 大对象高并发测试
<img width="713" height="631" alt="image" src="https://github.com/user-attachments/assets/b11132f3-a486-4e45-aace-74b9682241ac" />

### 结论
  - 较系统调用快了 100%





