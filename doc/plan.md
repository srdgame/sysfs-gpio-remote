远程 GPIO 映射工具

1. 工具有服务端（在有gpio的设备上运行，也是Linux系统)，客户端（运行在本机,系统为Linux)
2. 工具将服务端的gpio映射到客户端的主机上
3. 任何程序操作客户端的gpio，都会实时将操作映射到服务器端的gpio上。 同时读取gpio时，也会从服务器端读取gpio然后返回给应用程序。
4. 通讯使用TCP套接字
5. 命名 GPIO：支持 OpenWrt `gpio-export` 风格的命名引脚（如 `pcie_on`，
   非数字编号）。此类引脚在服务端 `/sys/class/gpio` 下表现为符号链接目录
   （仅含 `value`，内核驱动持有引脚），只支持 value 读写透传；若名字能匹配
   gpiod 线名（`gpio-line-names`），则提供全功能（direction/edge/active_low）。
6. 连接时枚举：客户端每次连接（含重连、客户端重启）向服务端查询已导出引脚
   并补齐本地目录视图，覆盖服务端残留导出。
7. 自动镜像：服务端 `/sys/class/gpio` 中预存的命名项在客户端每次连接时自动
   镜像为客户端目录中的命名引脚目录。


参考:
1. /home/cch/github/sysfs-gpio-shim 这里有使用fuse模拟文件系统gpio的代码
