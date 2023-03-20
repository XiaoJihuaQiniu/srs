# 说明
基于SRS 5.0release (https://github.com/ossrs/srs/tree/5.0release)

# 编译
cd trunk
./configure

# 运行
CANDIDATE=192.168.1.5 GATE_SERVER="127.0.0.1:10010" ./objs/srs -c ./conf/srs.conf
> 配置GATE_SERVER后，所有推流都会发布到GATE_SERVER，拉流都会从GATE_SERVER订阅。

CANDIDATE=192.168.1.5 ./objs/srs -c ./conf/srs.conf
> 测试时也可不配置GATE_SERVER。
