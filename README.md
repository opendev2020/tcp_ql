# SATCC
SATCC: A Congestion Control Algorithm for Dynamic Satellite Networks.

## Envs
os : Ubuntu 20/18，linux kernel-4.146-mptcp

## usage
clone the repo
```
git clone https://github.com/opendev2020/tcp_ql.git
```
build
```
cd SATCC
make all
```
insert the module
```
sudo insmod tcp_satcc.ko
```
set satcc as current congestion control
```
sysctl net.ipv4.tcp_congestion_control=satcc
```