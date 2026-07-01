首先需要连接上wifi：
1. ifconfig wlan0 up//使能网卡
2. wpa_supplicant -D nl80211 -i wlan0 -c /etc/wpa_supplicant.conf
-B//连接wifi，需要先修改/etc/wpa_supplicant.conf文件，填入wifi的ssid和密码
3. udhcpc -i wlan0//获取 IPv4 地址

确认网络没问题后，即可打开小核应用 ： ./rtsp_sender --fifo 0x12ffa000
然后可用VLC打开网络串流，地址为板子地址
例如： rtsp://10.239.114.28

然后即可启动大核程序

如果想要先验证RTSP流是否正常
可用文件模式打开RTSP流，例如在小核执行：
./rtsp_sender girlshy.h265
girlshy.h265为一个测试视频文件，可用VLC打开验证
最好和程序放在同一个目录下
