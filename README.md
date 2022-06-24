# tcpql
tcpql is a new congestion control algorithm based on q-learning.


## Envs
os : Ubuntu

## usage
clone the repo
```
git clone https://github.com/opendev2020/tcp_ql.git
```
build
```
cd tcp_ql
make all
```
insert the module
```
sudo insmod tcpql.ko
```
set tcpql as current congestion control
```
sysctl net.ipv4.tcp_congestion_control=tcpql
```