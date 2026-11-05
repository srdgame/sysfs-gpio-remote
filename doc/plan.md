远程 GPIO 映射工具

1. 工具有服务端（在有gpio的设备上运行，也是Linux系统)，客户端（运行在本机,系统为Linux)
2. 工具将服务端的gpio映射到客户端的主机上
3. 任何程序操作客户端的gpio，都会实时将操作映射到服务器端的gpio上。 同时读取gpio时，也会从服务器端读取gpio然后返回给应用程序。
4. 通讯使用TCP套接字


参考:
1. /home/cch/github/sysfs-gpio-shim 这里有使用fuse模拟文件系统gpio的代码
